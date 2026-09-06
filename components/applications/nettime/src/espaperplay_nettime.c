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

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "espaperplay_clock.h"
#include "espaperplay_geoip.h"
#include "espaperplay_netip.h"
#include "espaperplay_nettime.h"
#include "espaperplay_wifi.h"

static const char *TAG = "ESPaperPlay_NETTIME";

/** NTP 首次同步等待超时（毫秒）。 */
#define ESPAPERPLAY_NETTIME_NTP_SYNC_TIMEOUT_MS 10000
/** 网络时间同步任务栈大小（含 HTTPS/TLS 握手开销，与 main 任务一致）。 */
#define ESPAPERPLAY_NETTIME_TASK_STACK_SIZE 8192
/** 网络时间同步任务优先级。 */
#define ESPAPERPLAY_NETTIME_TASK_PRIORITY 3
/** 同步失败后的重试间隔（毫秒）。 */
#define ESPAPERPLAY_NETTIME_RETRY_INTERVAL_MS 30000
/** 等待网络就绪时的轮询间隔（毫秒，仅作为通知超时的兜底）。 */
#define ESPAPERPLAY_NETTIME_POLL_INTERVAL_MS 5000

static TaskHandle_t s_task = NULL;
static esp_event_handler_instance_t s_ip_instance = NULL;
static bool s_synced = false;

/** 判断当前是否已通过 STA 获取 IP（可上网）。 */
static bool nettime_wifi_sta_online(void) {
    espaperplay_wifi_status_t status;
    return espaperplay_wifi_get_status(&status) == ESP_OK && status.started && status.connected &&
           status.mode == ESPAPERPLAY_WIFI_MODE_STA;
}

/** IP 事件回调：STA 获取 IP 后立即唤醒同步任务，实现“联网后及时对时”。 */
static void nettime_on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id,
                              void *event_data) {
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
        ESP_LOGI(TAG, "got IP event, wake nettime task");
    }
}

/** 执行完整链路：公网 IP → 地理位置 → 时区 → NTP 同步。成功返回 ESP_OK。 */
static esp_err_t nettime_sync_chain(void) {
    /* 步骤 1：获取本机公网 IP。 */
    char ip[ESPAPERPLAY_NETIP_IP_MAX_LEN];
    esp_err_t err = espaperplay_netip_query(ip, sizeof(ip));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "step 1 (query public ip) failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 步骤 2：使用相同 IP 查询地理位置（含时区）。 */
    espaperplay_geoip_info_t geo;
    err = espaperplay_geoip_query(ip, &geo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "step 2 (query geolocation) failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 步骤 3：根据地理位置设置时区，并用 NTP 同步时间。 */
    if (geo.time_zone[0] != '\0') {
        err = espaperplay_clock_set_timezone(geo.time_zone);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "step 3 (set timezone) failed: %s", esp_err_to_name(err));
            return err;
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
        return err;
    }
    err = espaperplay_clock_ntp_wait_sync(ESPAPERPLAY_NETTIME_NTP_SYNC_TIMEOUT_MS);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "step 3 (wait ntp sync) failed: %s", esp_err_to_name(err));
        return err;
    }
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "step 3 (wait ntp sync) timed out after %u ms",
                 ESPAPERPLAY_NETTIME_NTP_SYNC_TIMEOUT_MS);
        return err;
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
        ESP_LOGI(TAG, "net time sync done: ip=%s region=\"%s\" tz=%s local=%s", geo.ip, geo.region,
                 tz, buf);
    } else {
        ESP_LOGI(TAG, "net time sync done: ip=%s region=\"%s\" tz=%s", geo.ip, geo.region, tz);
    }
    return ESP_OK;
}

/** 网络时间同步任务：常驻后台，断网时等待、联网后立即对时，失败自动重试。 */
static void nettime_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "net time sync task started");

    while (1) {
        /* 等待 STA 联网：未联网时阻塞等待 IP 事件通知，超时则轮询一次，避免事件丢失。 */
        while (!nettime_wifi_sta_online()) {
            ESP_LOGD(TAG, "waiting for STA network...");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ESPAPERPLAY_NETTIME_POLL_INTERVAL_MS));
        }

        ESP_LOGI(TAG, "STA online, starting time sync chain");
        esp_err_t err = nettime_sync_chain();
        if (err == ESP_OK) {
            s_synced = true;
            ESP_LOGI(TAG, "net time sync succeeded, waiting for next trigger");
            /* 已成功：进入长等待，由 IP 事件或外部 request_sync 唤醒后再次同步
             *（例如 IP 变更导致时区变化）。使用 portMAX_DELAY 避免空转。 */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            ESP_LOGI(TAG, "nettime task woken for re-sync");
            /* 被唤醒后若仍在线则立即重走链路，否则回到外层等待联网 */
            continue;
        }

        /* 失败：等待重试间隔，期间若网络恢复（IP 事件）可提前唤醒重试 */
        ESP_LOGW(TAG, "net time sync failed (%s), retry in %u ms", esp_err_to_name(err),
                 ESPAPERPLAY_NETTIME_RETRY_INTERVAL_MS);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ESPAPERPLAY_NETTIME_RETRY_INTERVAL_MS));
        /* 若在此期间网络已断开，下次循环会回到等待联网分支 */
    }
}

esp_err_t espaperplay_nettime_start(void) {
    if (s_task != NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(nettime_task, "nettime_task", ESPAPERPLAY_NETTIME_TASK_STACK_SIZE, NULL,
                    ESPAPERPLAY_NETTIME_TASK_PRIORITY, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create nettime task");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 注册 IP 获取事件，网络连接后及时唤醒任务（事件在 wifi_init 之后才可能触发，
     * 此处注册可覆盖后续所有联网）。 */
    if (s_ip_instance == NULL) {
        esp_err_t err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &nettime_on_got_ip, NULL, &s_ip_instance);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to register IP_EVENT handler: %s", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "net time sync task created");
    return ESP_OK;
}

void espaperplay_nettime_request_sync(void) {
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}
