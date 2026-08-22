/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "espaperplay_clock.h"
#include "espaperplay_geoip.h"
#include "espaperplay_nettime.h"
#include "espaperplay_netip.h"
#include "espaperplay_wifi.h"

static const char *TAG = "ESPaperPlay_NETTIME";

/** 等待 WiFi STA 联网的最长时间（毫秒）。 */
#define ESPAPERPLAY_NETTIME_WIFI_WAIT_MS 60000
/** WiFi 状态轮询间隔（毫秒）。 */
#define ESPAPERPLAY_NETTIME_WIFI_POLL_MS 1000
/** NTP 首次同步等待超时（毫秒）。 */
#define ESPAPERPLAY_NETTIME_NTP_SYNC_TIMEOUT_MS 10000
/** 网络时间同步任务栈大小（含 HTTPS/TLS 握手开销，与 main 任务一致）。 */
#define ESPAPERPLAY_NETTIME_TASK_STACK_SIZE 8192
/** 网络时间同步任务优先级。 */
#define ESPAPERPLAY_NETTIME_TASK_PRIORITY 3

/** 等待 WiFi 以 STA 模式联网；返回 true 表示网络可用。 */
static bool nettime_wait_wifi_connected(void) {
    int waited = 0;
    while (waited < ESPAPERPLAY_NETTIME_WIFI_WAIT_MS) {
        espaperplay_wifi_status_t status;
        if (espaperplay_wifi_get_status(&status) == ESP_OK && status.started && status.connected &&
            status.mode == ESPAPERPLAY_WIFI_MODE_STA) {
            ESP_LOGI(TAG, "wifi connected (sta, ip=%s)", status.ip);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_NETTIME_WIFI_POLL_MS));
        waited += ESPAPERPLAY_NETTIME_WIFI_POLL_MS;
    }
    ESP_LOGW(TAG, "wifi not connected after %u ms (AP mode or no network), skip net time sync",
             ESPAPERPLAY_NETTIME_WIFI_WAIT_MS);
    return false;
}

/** 执行完整链路：公网 IP → 地理位置 → 时区 → NTP 同步。 */
static void nettime_sync_chain(void) {
    /* 步骤 1：获取本机公网 IP。 */
    char ip[ESPAPERPLAY_NETIP_IP_MAX_LEN];
    esp_err_t err = espaperplay_netip_query(ip, sizeof(ip));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "step 1 (query public ip) failed: %s", esp_err_to_name(err));
        return;
    }

    /* 步骤 2：使用相同 IP 查询地理位置（含时区）。 */
    espaperplay_geoip_info_t geo;
    err = espaperplay_geoip_query(ip, &geo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "step 2 (query geolocation) failed: %s", esp_err_to_name(err));
        return;
    }

    /* 步骤 3：根据地理位置设置时区，并用 NTP 同步时间。 */
    if (geo.time_zone[0] != '\0') {
        err = espaperplay_clock_set_timezone(geo.time_zone);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "step 3 (set timezone) failed: %s", esp_err_to_name(err));
            return;
        }
    } else {
        ESP_LOGW(TAG, "geolocation has no timezone info");
        char tz[ESPAPERPLAY_CLOCK_TZ_MAX_LEN];
        if (espaperplay_clock_get_timezone(tz, sizeof(tz)) == ESP_OK) {
            ESP_LOGW(TAG, "keeping current timezone \"%s\"", tz);
        }
    }

    err = espaperplay_clock_ntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "step 3 (start ntp) failed: %s", esp_err_to_name(err));
        return;
    }
    err = espaperplay_clock_ntp_wait_sync(ESPAPERPLAY_NETTIME_NTP_SYNC_TIMEOUT_MS);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "step 3 (wait ntp sync) failed: %s", esp_err_to_name(err));
        return;
    }

    /* 记录本次成功对时，初始化漂移标定模型的校正基准与测量基线。 */
    espaperplay_clock_mark_synced();

    /* 输出结果：地理位置 + 时区 + 本地时间。 */
    struct tm local_time;
    char tz[ESPAPERPLAY_CLOCK_TZ_MAX_LEN];
    espaperplay_clock_get_timezone(tz, sizeof(tz));
    if (espaperplay_clock_get_local_time(&local_time) == ESP_OK) {
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z (%z)", &local_time);
        ESP_LOGI(TAG, "net time sync done: ip=%s region=\"%s\" tz=%s local=%s", geo.ip,
                 geo.region, tz, buf);
    } else {
        ESP_LOGI(TAG, "net time sync done: ip=%s region=\"%s\" tz=%s", geo.ip, geo.region, tz);
    }
}

/** 网络时间同步任务：等待联网后执行完整链路。 */
static void nettime_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "net time sync task started");

    if (!nettime_wait_wifi_connected()) {
        ESP_LOGI(TAG, "net time sync task exiting (no network)");
        vTaskDelete(NULL);
        return;
    }

    nettime_sync_chain();
    ESP_LOGI(TAG, "net time sync task exiting");
    vTaskDelete(NULL);
}

esp_err_t espaperplay_nettime_start(void) {
    static bool s_started = false; /* 幂等：只允许创建一次任务 */
    if (s_started) {
        return ESP_OK;
    }

    if (xTaskCreate(nettime_task, "nettime_task", ESPAPERPLAY_NETTIME_TASK_STACK_SIZE, NULL,
                    ESPAPERPLAY_NETTIME_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create nettime task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "net time sync task created");
    return ESP_OK;
}
