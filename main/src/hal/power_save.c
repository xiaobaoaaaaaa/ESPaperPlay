#include "power_save.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <time.h>
#include "wifi_ctrl.h"
#include "vars.h"
#include "sntp.h"
#include "lvgl_init.h"

#define POWER_SAVE_BIT  BIT0
#define POWER_SAVE_TIMEOUT_MIN  3
#define TAG "power_save"

static EventGroupHandle_t pwr_save_event_group ;
static int no_activity_minutes = 0;
esp_timer_handle_t inactivity_timer;

void inactivity_timer_callback(void* arg) {
    no_activity_minutes++;
}

void reset_inactivity_timer() {
    no_activity_minutes = 0;
}

void start_inactivity_timer() {
    const esp_timer_create_args_t timer_args = {
        .callback = &inactivity_timer_callback,
        .name = "inactivity_minute_timer"
    };
    esp_timer_create(&timer_args, &inactivity_timer);
    esp_timer_start_periodic(inactivity_timer, 60 * 1000 * 1000);  // 每分钟（单位：微秒）
}

int time_correction_count = 0;
bool wifi_off_autoly = false; //标记自动睡眠程序是否关闭了WiFi
void sleep_wakeup()
{
    ESP_LOGI(TAG, "Entering sleep mode");
    time_correction_count++;
    set_var_is_power_save(true);
    //关闭wifi
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if(bits & BIT0) {
        ESP_LOGI(TAG, "WiFi disconnecting...");
        wifi_off_autoly = true;
        esp_wifi_disconnect();
        esp_wifi_stop();
        set_wifi_on_off(false);
    }
    xEventGroupClearBits(lvgl_flush_event_group, BIT0);
    vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒，确保WiFi断开
    esp_light_sleep_start();

    ESP_LOGI(TAG, "Woke up from sleep mode");
    xEventGroupSetBits(lvgl_flush_event_group, BIT0);
    gpio_wakeup_disable(GPIO_NUM_4);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "Woken up by GPIO interrupt");
        set_var_is_power_save(false);
        //重新连接WiFi
        if(!wifi_manually_stopped && wifi_off_autoly)
        {
            wifi_off_autoly = false;
            esp_wifi_start();
            esp_wifi_connect();
            set_wifi_on_off(true);
        }
        reset_inactivity_timer();
    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Woken up by timer");
        vTaskDelay(pdMS_TO_TICKS(2000)); // 等待1秒，确保系统稳定
    } else {
        ESP_LOGI(TAG, "Woken up by unknown cause: %d", cause);
    }

    if(time_correction_count >= 60)
    {
        time_correction_count = 0;
        ESP_LOGI(TAG, "Time correction triggered");
        esp_wifi_start();
        esp_wifi_connect();
        set_wifi_on_off(true);
        int retry_count = 0;
        bits = xEventGroupGetBits(s_wifi_event_group);
        while (!(bits & BIT0)) {
            if(retry_count++ > 10)
            {
                ESP_LOGE(TAG, "Cannot obtain time: Network Error");
                time_correction_count = 30;  //将同步间隔缩短为半小时
                return;
            }
            ESP_LOGI(TAG, "Waiting for WiFi to connect...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            bits = xEventGroupGetBits(s_wifi_event_group);
        }
        esp_sntp_restart();
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "Time corrected after waking up from sleep");
        esp_wifi_disconnect();
        esp_wifi_stop();
        set_wifi_on_off(false);
    }
}

void power_save(void *param)
{
    ESP_LOGI(TAG, "Initializing power save mode");
    while (1)
    {
        gpio_wakeup_enable(GPIO_NUM_4, GPIO_INTR_LOW_LEVEL);
        //gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        // 获取当前时间
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        
        // 计算距下一分钟的秒数
        int seconds_to_next_minute = 60 - timeinfo.tm_sec;
        esp_sleep_enable_timer_wakeup(1000000 * seconds_to_next_minute); // 设置唤醒时间为下一分钟

        xEventGroupWaitBits(pwr_save_event_group, POWER_SAVE_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
        //无操作三分钟后激活睡眠
        if (no_activity_minutes >= POWER_SAVE_TIMEOUT_MIN) 
        {
            ESP_LOGI(TAG, "No activity for %d minutes, entering deep sleep", POWER_SAVE_TIMEOUT_MIN);
            sleep_wakeup();
        }
        else
        {
            ESP_LOGI(TAG, "Will enter light sleep in %d minutes", POWER_SAVE_TIMEOUT_MIN - no_activity_minutes);
            vTaskDelay(pdMS_TO_TICKS(1000 * 61));
        }
    }
    
}

void power_save_init(void)
{
    pwr_save_event_group = xEventGroupCreate();
    xEventGroupSetBits(pwr_save_event_group, POWER_SAVE_BIT); // Initialize the event group with no bits set
    start_inactivity_timer();
    xTaskCreate(power_save, "power_save_task", 4096, NULL, 15, NULL);
}