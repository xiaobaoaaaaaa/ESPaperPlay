/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"

#include "espaperplay_clock.h"

static const char *TAG = "ESPaperPlay_CLOCK";

/** NVS 命名空间。 */
#define ESPAPERPLAY_CLOCK_NVS_NAMESPACE "clock"
/** NVS 中保存时区名称的键。 */
#define ESPAPERPLAY_CLOCK_NVS_KEY_TZ "tz"

/** 默认 NTP 服务器列表（按优先级排序，需 CONFIG_LWIP_SNTP_MAX_SERVERS >= 3）。 */
static const char *s_ntp_servers[ESPAPERPLAY_CLOCK_NTP_SERVER_COUNT] = {
    "ntp.aliyun.com",
    "cn.pool.ntp.org",
    "pool.ntp.org",
};

/** 当前生效的时区名称（内存缓存）。 */
static char s_tz_name[ESPAPERPLAY_CLOCK_TZ_MAX_LEN] = ESPAPERPLAY_CLOCK_DEFAULT_TZ;

/** SNTP 是否已初始化启动。 */
static bool s_sntp_started = false;

/* ------------------------------------------------------------------ */
/* 时区                                                                */
/* ------------------------------------------------------------------ */

/** 应用时区到 libc（setenv("TZ") + tzset()）。 */
static void clock_apply_timezone(const char *tz_name) {
    setenv("TZ", tz_name, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set to \"%s\"", tz_name);
}

/** 从 NVS 读取持久化的时区名称；不存在或读取失败返回 false。 */
static bool clock_load_timezone(char *tz_out, size_t tz_out_len) {
    nvs_handle_t handle;
    if (nvs_open(ESPAPERPLAY_CLOCK_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(handle, ESPAPERPLAY_CLOCK_NVS_KEY_TZ, tz_out, &tz_out_len) == ESP_OK;
    nvs_close(handle);
    return ok;
}

/** 把时区名称持久化到 NVS（失败仅告警，不影响本次生效）。 */
static void clock_store_timezone(const char *tz_name) {
    nvs_handle_t handle;
    if (nvs_open(ESPAPERPLAY_CLOCK_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "failed to open nvs namespace \"%s\"", ESPAPERPLAY_CLOCK_NVS_NAMESPACE);
        return;
    }
    esp_err_t err = nvs_set_str(handle, ESPAPERPLAY_CLOCK_NVS_KEY_TZ, tz_name);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist timezone \"%s\": %s", tz_name, esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------ */
/* NTP                                                                */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_clock_ntp_start(void) {
    if (s_sntp_started) {
        return ESP_OK;
    }

    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(ESPAPERPLAY_CLOCK_NTP_SERVER_COUNT,
                                               ESP_SNTP_SERVER_LIST(s_ntp_servers[0],
                                                                    s_ntp_servers[1],
                                                                    s_ntp_servers[2]));
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to init sntp: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start sntp: %s", esp_err_to_name(err));
        return err;
    }

    s_sntp_started = true;
    ESP_LOGI(TAG, "sntp started (servers: %s, %s, %s)", s_ntp_servers[0], s_ntp_servers[1],
             s_ntp_servers[2]);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_clock_init(void) {
    char tz[ESPAPERPLAY_CLOCK_TZ_MAX_LEN] = ESPAPERPLAY_CLOCK_DEFAULT_TZ;
    if (clock_load_timezone(tz, sizeof(tz))) {
        ESP_LOGI(TAG, "restored persisted timezone \"%s\"", tz);
    } else {
        ESP_LOGI(TAG, "no persisted timezone, using default \"%s\"", tz);
    }
    strlcpy(s_tz_name, tz, sizeof(s_tz_name));
    clock_apply_timezone(s_tz_name);
    return ESP_OK;
}

esp_err_t espaperplay_clock_set_timezone(const char *tz_name) {
    const char *tz = (tz_name == NULL || tz_name[0] == '\0') ? ESPAPERPLAY_CLOCK_DEFAULT_TZ
                                                             : tz_name;
    if (strlen(tz) >= sizeof(s_tz_name)) {
        ESP_LOGE(TAG, "timezone name too long: \"%s\"", tz);
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_tz_name, tz, sizeof(s_tz_name));
    clock_apply_timezone(s_tz_name);
    clock_store_timezone(s_tz_name);
    return ESP_OK;
}

esp_err_t espaperplay_clock_get_timezone(char *tz_out, size_t tz_out_len) {
    if (tz_out == NULL || tz_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(tz_out, s_tz_name, tz_out_len);
    return ESP_OK;
}

esp_err_t espaperplay_clock_ntp_wait_sync(uint32_t timeout_ms) {
    if (!s_sntp_started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "time synchronized via ntp");
    } else if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "ntp sync timed out after %u ms", timeout_ms);
    } else {
        ESP_LOGW(TAG, "ntp sync wait returned %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t espaperplay_clock_get_local_time(struct tm *local_time) {
    if (local_time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    time_t now = time(NULL);
    localtime_r(&now, local_time);
    return ESP_OK;
}
