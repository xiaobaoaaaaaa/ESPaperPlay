#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_ctrl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include"sntp.h"
#include <time.h>
#include "button.h"
#include "driver/gpio.h"
#include "buzzer.h"
#include "lvgl_init.h"
#include "touch.h"
#include "power_save.h"
#include "yiyan.h"
#include <esp_task_wdt.h>
#include "actions.h"
#include <time.h>
#include <sys/time.h>
#include "date_update.h"
#include "config_manager.h"

#define TAG "main"

EventGroupHandle_t init_event_group;

#define WIFI_INIT_BIT      BIT0
#define TIME_INIT_BIT      BIT1
#define BUTTON_INIT_BIT    BIT2
#define BUZZER_INIT_BIT    BIT3
#define TOUCH_INIT_BIT     BIT4
#define DISP_INIT_BIT      BIT5
#define SLEEP_INIT_BIT     BIT6


void wifi_init_task(void *param)
{
    bool success = wifi_init();
    if (success) {
        ESP_LOGI(TAG, "WiFi initialization successful.");
    } else {
        ESP_LOGE(TAG, "WiFi initialization failed.");
    }
    xEventGroupSetBits(init_event_group, WIFI_INIT_BIT);
    vTaskDelete(NULL); // 任务完成后删除自身
}

void time_init_task(void *param)
{
    xEventGroupWaitBits(init_event_group, WIFI_INIT_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    time_init();
    //打印系统时间
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s", strftime_buf);
    xEventGroupSetBits(init_event_group, TIME_INIT_BIT);
    vTaskDelete(NULL); // 任务完成后删除自身
}

void button_init_task(void *param)
{
    button_init(GPIO_NUM_0);
    xEventGroupSetBits(init_event_group, BUTTON_INIT_BIT);
    vTaskDelete(NULL); // 任务完成后删除自身
}

void buzzer_init_task(void *param)
{
    buzzer_init(15); // 假设蜂鸣器连接在 GPIO 15

    //鸣响蜂鸣器三次
    buzzer(NOTE_F7, 6000, 0.3, 0, 1);
    buzzer(NOTE_G7, 6000, 0.3, 0, 1);
    buzzer(NOTE_A7, 6000, 0.3, 0, 1);
    
    xEventGroupSetBits(init_event_group, BUZZER_INIT_BIT);
    vTaskDelete(NULL); // 任务完成后删除自身
}

void touch_init_task(void *param)
{
    sd_touch_init();
    xEventGroupSetBits(init_event_group, TOUCH_INIT_BIT);
    vTaskDelete(NULL); // 任务完成后删除自身
}

void epaper_init_task(void *param)
{
    xEventGroupWaitBits(init_event_group, WIFI_INIT_BIT | TIME_INIT_BIT | TOUCH_INIT_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    lvgl_init_epaper_display();
    xEventGroupSetBits(init_event_group, DISP_INIT_BIT);
    vTaskDelete(NULL); // 初始化完成后删除自身
}

void power_save_init_task(void *param)
{
    power_save_init();
    xEventGroupSetBits(init_event_group, SLEEP_INIT_BIT);
    vTaskDelete(NULL);
}

void time_tick_task(void *arg)
{
    xEventGroupWaitBits(init_event_group, DISP_INIT_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    date_update();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        date_update();  // 每分钟整点触发
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Hello! System booting...");
    
    // 初始化任务看门狗
    esp_task_wdt_config_t cfg = {
        .timeout_ms = 10000,         // 设置超时时长
        .idle_core_mask = BIT(0)|BIT(1),
        .trigger_panic = true        // 触发 panic
    };
    esp_task_wdt_init(&cfg);

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

    //初始化系统配置
    config_manager_init();

    init_event_group = xEventGroupCreate();

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

    // 初始化触摸屏
    ESP_LOGI(TAG, "Initializing touch screen...");
    xTaskCreate(touch_init_task, "touch_init_task", 4096, NULL, 5, NULL);

    // 初始化 e-Paper显示
    ESP_LOGI(TAG, "Initializing e-Paper display...");
    xTaskCreate(epaper_init_task, "epaper_init_task", 4096, NULL, 5, NULL); // 将 e-Paper 初始化任务绑定到核心 1

    // 初始化睡眠
    ESP_LOGI(TAG, "Initializing power save mode...");
    xTaskCreate(power_save_init_task, "power_save_init_task", 4096, NULL, 5, NULL);

    // 初始化时间更新
    ESP_LOGI(TAG, "Initializing date-update task...");
    xTaskCreate(time_tick_task, "time_tick_task", 2048, NULL, 5, NULL);

    return;
}