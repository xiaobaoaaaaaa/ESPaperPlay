#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_ctrl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include"sntp.h"
#include <time.h>
#include "button.h"
#include "driver/gpio.h"
#include "buzzer.h"
#include "lvgl_init.h"
#include "touch.h"
#include "power_save.h"
#include "yiyan.h"

#define TAG "main"

void wifi_init_task(void *param)
{
    bool success = wifi_init();
    if (success) {
        ESP_LOGI(TAG, "WiFi initialization successful.");
    } else {
        ESP_LOGE(TAG, "WiFi initialization failed.");
    }
    vTaskDelete(NULL); // 任务完成后删除自身
}

void time_init_task(void *param)
{
    time_init();
    //打印系统时间
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s", strftime_buf);
    vTaskDelete(NULL); // 任务完成后删除自身
}

void button_init_task(void *param)
{
    button_init(GPIO_NUM_0);
    vTaskDelete(NULL); // 任务完成后删除自身
}

void buzzer_init_task(void *param)
{
    buzzer_init(15); // 假设蜂鸣器连接在 GPIO 15
    vTaskDelete(NULL); // 任务完成后删除自身
}

void epaper_init_task(void *param)
{
    lvgl_init_epaper_display();
    vTaskDelete(NULL); // 初始化完成后删除自身
}

void power_save_init_task(void *param)
{
    power_save_init();
}

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
    ESP_LOGI(TAG, "Starting WiFi initialization task...");
    xTaskCreate(wifi_init_task, "wifi_init_task", 4096, NULL, 5, NULL);

    // 初始化时间同步
    ESP_LOGI(TAG, "Starting time initialization...");
    xTaskCreate(time_init_task, "time_init_task", 4096, NULL, 5, NULL);

    // 初始化按钮
    ESP_LOGI(TAG, "Initializing button...");
    xTaskCreate(button_init_task, "button_init_task", 4096, NULL, 5, NULL);

    // 初始化蜂鸣器
    ESP_LOGI(TAG, "Initializing buzzer...");
    xTaskCreate(buzzer_init_task, "buzzer_init_task", 4096, NULL, 5, NULL);

    // 初始化 e-Paper显示
    ESP_LOGI(TAG, "Initializing e-Paper display...");
    xTaskCreate(epaper_init_task, "epaper_init_task", 4096, NULL, 5, NULL); // 将 e-Paper 初始化任务绑定到核心 1

    // 初始化触摸屏
    ESP_LOGI(TAG, "Initializing touch screen...");
    sd_touch_init();

    // 初始化睡眠
    ESP_LOGI(TAG, "Initializing power save mode...");
    xTaskCreate(power_save_init_task, "power_save_init_task", 4096, NULL, 5, NULL);
}