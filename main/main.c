#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_ctrl.h"

#define TAG "main"

void app_main(void)
{
    ESP_LOGI(TAG, "Hello! System booting...");

    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 需要擦除
        ESP_LOGW(TAG, "NVS needs to be erased, erasing now...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized successfully.");

    // 初始化 WiFi
    bool success = wifi_init();
    if (success) {
        ESP_LOGI(TAG, "WiFi initialization successful.");
    } else {
        ESP_LOGE(TAG, "WiFi initialization failed.");
    }
}