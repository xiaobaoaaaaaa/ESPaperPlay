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

/** 当前已应用到 libc 的 POSIX TZ 字符串（默认 UTC）。 */
static char s_posix_tz[ESPAPERPLAY_CLOCK_TZ_MAX_LEN] = "UTC0";

/** SNTP 是否已初始化启动。 */
static bool s_sntp_started = false;

/* ------------------------------------------------------------------ */
/* 时区                                                                */
/* ------------------------------------------------------------------ */

/**
 * IANA 时区名 -> POSIX TZ 字符串映射表。
 *
 * ESP-IDF 的 libc（newlib / picolibc）tzset() 只识别 POSIX 格式的 TZ 字符串
 * （如 "CST-8"），不支持 IANA 时区名（如 "Asia/Shanghai"）——传 IANA 名会
 * 解析失败并静默回退 UTC。因此 geoip 返回的 IANA 时区必须先翻译成 POSIX
 * 格式再应用。未收录的时区保持当前时区并告警（见 clock_apply_timezone）。
 *
 * POSIX TZ 语法：std offset [dst [offset2] [,start[/time],end[/time]]]，
 * 其中 offset 为「本地时间加该值得到 UTC」，符号与常识相反：西正东负，
 * 故 UTC+8（中国）写作 "CST-8"。
 */
typedef struct {
    const char *iana;  /*!< IANA 时区名（geoip 返回 / NVS 持久化的形式） */
    const char *posix; /*!< 对应的 POSIX TZ 字符串（tzset() 实际使用的形式） */
} clock_tz_map_entry_t;

static const clock_tz_map_entry_t s_tz_map[] = {
    /* ---- 固定偏移（无夏令时） ---- */
    {"UTC", "UTC0"},
    {"Etc/UTC", "UTC0"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Taipei", "CST-8"},
    {"Asia/Macau", "CST-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Seoul", "KST-9"},
    {"Asia/Singapore", "SGT-8"},
    {"Asia/Kuala_Lumpur", "MYT-8"},
    {"Asia/Bangkok", "ICT-7"},
    {"Asia/Ho_Chi_Minh", "ICT-7"},
    {"Asia/Jakarta", "WIB-7"},
    {"Asia/Manila", "PHT-8"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Dhaka", "BST-6"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Dubai", "GST-4"},
    {"Asia/Riyadh", "AST-3"},
    {"Asia/Baghdad", "AST-3"},
    {"Asia/Tehran", "IST-3:30"},
    {"Asia/Yangon", "MMT-6:30"},
    {"Asia/Jerusalem", "IST-2"},
    {"Europe/Moscow", "MSK-3"},
    {"Europe/Istanbul", "TRT-3"},
    {"Europe/Kyiv", "EET-2"},
    {"Europe/Athens", "EET-2"},
    {"Europe/Helsinki", "EET-2"},
    {"Europe/Bucharest", "EET-2"},
    {"Europe/Riga", "EET-2"},
    {"Europe/Vilnius", "EET-2"},
    {"Europe/Tallinn", "EET-2"},
    {"Africa/Cairo", "EET-2"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Lagos", "WAT-1"},
    {"Africa/Nairobi", "EAT-3"},
    {"Australia/Perth", "AWST-8"},
    {"Australia/Brisbane", "AEST-10"},
    {"Pacific/Honolulu", "HST10"},
    {"Pacific/Guam", "ChST-10"},
    {"Pacific/Fiji", "FJT-12"},
    {"America/Phoenix", "MST7"},
    {"America/Mexico_City", "CST6"},
    {"America/Puerto_Rico", "AST4"},
    {"America/Sao_Paulo", "BRT+3"},
    {"America/Argentina/Buenos_Aires", "ART+3"},
    {"America/Bogota", "COT+5"},
    {"America/Lima", "PET+5"},
    {"America/Caracas", "VET+4"},
    {"America/Santiago", "CLT+4"},
    /* ---- 北美（3 月第二个周日 / 11 月第一个周日，2:00 本地） ---- */
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Montreal", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Winnipeg", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Edmonton", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"},
    /* ---- 欧洲（欧盟统一规则：3 月最后周日 / 10 月最后周日） ---- */
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Belgrade", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    /* ---- 澳洲 / 新西兰 ---- */
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

/** IANA 时区名 -> POSIX TZ 字符串；未收录返回 NULL。 */
static const char *clock_iana_to_posix(const char *iana) {
    for (size_t i = 0; i < sizeof(s_tz_map) / sizeof(s_tz_map[0]); i++) {
        if (strcmp(s_tz_map[i].iana, iana) == 0) {
            return s_tz_map[i].posix;
        }
    }
    return NULL;
}

/** 应用时区到 libc（setenv("TZ") + tzset()，仅支持 POSIX 格式）。 */
static void clock_apply_timezone(const char *tz_name) {
    const char *posix = clock_iana_to_posix(tz_name);
    if (posix == NULL) {
        /* 未收录的时区：保持当前已生效的时区，避免误切到 UTC。 */
        ESP_LOGW(TAG, "IANA timezone \"%s\" has no POSIX mapping, keeping \"%s\"", tz_name,
                 s_posix_tz);
        return;
    }
    strlcpy(s_posix_tz, posix, sizeof(s_posix_tz));
    setenv("TZ", s_posix_tz, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set to \"%s\" (posix \"%s\")", tz_name, s_posix_tz);
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
