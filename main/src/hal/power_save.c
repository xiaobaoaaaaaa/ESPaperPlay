#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "config_manager.h"
#include "lvgl_init.h"
#include "power_save.h"
#include "sntp.h"
#include "vars.h"
#include "wifi_ctrl.h"

#define POWER_SAVE_BIT  BIT0
#define TAG "power_save"

EventGroupHandle_t pwr_save_event_group;
static int no_activity_minutes = 0;
esp_timer_handle_t inactivity_timer;
static bool power_save_enabled = true;
static int power_save_min = 3;

// 无操作计时器回调函数，每分钟触发一次
void inactivity_timer_callback(void* arg) 
{
    no_activity_minutes++;
}

void reset_inactivity_timer() 
{
    no_activity_minutes = 0;
}

// 启动无操作计时器
void start_inactivity_timer() 
{
    const esp_timer_create_args_t timer_args = {
        .callback = &inactivity_timer_callback,
        .name = "inactivity_minute_timer"
    };
    esp_timer_create(&timer_args, &inactivity_timer);
    esp_timer_start_periodic(inactivity_timer, 60 * 1000 * 1000);  // 每分钟（单位：微秒）
}

int time_correction_count = 0;
bool wifi_auto_disabled = false; // 标记自动睡眠程序是否关闭了WiFi
// 睡眠--唤醒逻辑
void sleep_wakeup()
{
    ESP_LOGI(TAG, "Entering sleep mode");
    time_correction_count++;
    set_var_is_power_save(true); // 通知UI进入了睡眠状态
    
    // 关闭wifi
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if (bits & BIT0) 
    {
        ESP_LOGI(TAG, "Disconnecting WiFi...");
        wifi_auto_disabled = true;
        esp_wifi_disconnect();
        esp_wifi_stop();
        set_wifi_on_off(false);
    }
    
    // 停止LVGL刷新事件
    xEventGroupClearBits(lvgl_flush_event_group, BIT0);
    vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒，确保WiFi断开
    
    // 处理用户在即将进入睡眠时触摸的情况
    if (no_activity_minutes < power_save_min) 
    {
        ESP_LOGW(TAG, "Skipping sleep due to recent activity");
        set_var_is_power_save(false);
        
        // 重新连接WiFi
        if (wifi_auto_disabled) 
        {
            wifi_auto_disabled = false;
            esp_wifi_start();
            esp_wifi_connect();
            set_wifi_on_off(true);
        }
        reset_inactivity_timer();
        return;
    }
    
    esp_light_sleep_start();

    // 此处从睡眠中唤醒
    ESP_LOGI(TAG, "Woke up from sleep mode");
    xEventGroupSetBits(lvgl_flush_event_group, BIT0);
    gpio_wakeup_disable(GPIO_NUM_4);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO) // 由触摸或者按键唤醒
    {
        ESP_LOGI(TAG, "Woken up by GPIO interrupt");
        set_var_is_power_save(false);
        
        // 重新连接WiFi
        if (wifi_auto_disabled) 
        {
            wifi_auto_disabled = false;
            esp_wifi_start();
            esp_wifi_connect();
            set_wifi_on_off(true);
        }
        reset_inactivity_timer();
    } 
    else if (cause == ESP_SLEEP_WAKEUP_TIMER) // 由定时器唤醒，刷新时间然后继续睡眠
    {
        ESP_LOGI(TAG, "Woken up by timer");
        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒，确保系统稳定
    } 
    else 
    {
        ESP_LOGI(TAG, "Woken up by unknown cause: %d", cause); // Fixed spelling
    }

    // 睡眠超过一定时间后进行时间校正
    if (time_correction_count >= 60) 
    {
        time_correction_count = 0;
        ESP_LOGI(TAG, "Time correction triggered");
        
        esp_wifi_start();
        esp_wifi_connect();
        set_wifi_on_off(true);
        
        // 等待WIFI连接成功
        int retry_count = 0;
        bits = xEventGroupGetBits(s_wifi_event_group);
        while (!(bits & BIT0)) 
        {
            if (retry_count++ > 10) 
            {
                ESP_LOGE(TAG, "Time sync failed: network error");
                time_correction_count = 30;  // 如果同步失败，将下一次同步间隔缩短为半小时
                return;
            }
            ESP_LOGI(TAG, "Waiting for WiFi connection...");
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
        if (!power_save_enabled) 
        {
            vTaskDelay(pdMS_TO_TICKS(30 * 1000));
            continue;
        }
        
        gpio_wakeup_enable(GPIO_NUM_4, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        // 获取当前时间
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        
        // 计算距下一分钟的秒数，并设置定时唤醒
        int seconds_to_next_minute = 60 - timeinfo.tm_sec;
        esp_sleep_enable_timer_wakeup(1000000 * seconds_to_next_minute);

        xEventGroupWaitBits(pwr_save_event_group, POWER_SAVE_BIT, 
                           pdFALSE, pdFALSE, portMAX_DELAY);
        
        // 无操作power_save_min分钟后激活睡眠
        if (no_activity_minutes >= power_save_min) 
        {
            ESP_LOGI(TAG, "No activity for %d minutes, entering sleep", power_save_min);
            sleep_wakeup();
        } 
        else 
        {
            ESP_LOGI(TAG, "Will enter sleep in %d minutes", 
                     power_save_min - no_activity_minutes);
            vTaskDelay(pdMS_TO_TICKS(1000 * 61));
        }
    }
}

// 初始化浅睡眠功能
void power_save_init(void)
{
    // 从配置中获取睡眠设置
    const system_config_t *cfg = config_get();
    power_save_enabled = cfg->power_save_enabled;
    
    if (!power_save_enabled) 
    {
        ESP_LOGW(TAG, "Power save disabled");
    }
    
    power_save_min = cfg->power_save_min;
    if (power_save_min < 1) 
    {
        power_save_min = 3;
        ESP_LOGW(TAG, "Invalid power save min, reset to %d", power_save_min); // Fixed spelling
    }
    
    pwr_save_event_group = xEventGroupCreate();
    xEventGroupSetBits(pwr_save_event_group, POWER_SAVE_BIT);
    start_inactivity_timer();
    xTaskCreatePinnedToCore(power_save, "power_save_task", 4096, NULL, 15, NULL, 0);
}