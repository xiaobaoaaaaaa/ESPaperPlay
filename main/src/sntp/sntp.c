#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sntp.h"

#include "wifi_ctrl.h"

static const char *TAG = "sntp";

void obtain_time(void);
static void initialize_sntp(void);

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronization event notification received"); // Improved phrasing
}

void time_init(void)
{
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    xEventGroupWaitBits(s_wifi_event_group, BIT0, pdFALSE, pdTRUE, portMAX_DELAY);

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year < (2021 - 1900)) {
        ESP_LOGI(TAG, "Time not set, connecting to WiFi for NTP time sync");
        obtain_time();
        time(&now);
        localtime_r(&now, &timeinfo);

        setenv("TZ", "CST-8", 1);
        tzset();
    }
}

void obtain_time(void)
{
    initialize_sntp();

    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time sync (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    time(&now);
    localtime_r(&now, &timeinfo);

    setenv("TZ", "CST-8", 1);
    tzset();
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}