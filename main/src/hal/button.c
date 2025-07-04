#include "button.h"
/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "iot_button.h"
#include "esp_sleep.h"
#include "esp_idf_version.h"
#include "button_gpio.h"
#include "buzzer.h"

/* Most development boards have "boot" button attached to GPIO0.
 * You can also change this to another pin.
 */
#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32C6
#define BOOT_BUTTON_NUM         9
#else
#define BOOT_BUTTON_NUM         0
#endif
#define BUTTON_ACTIVE_LEVEL     0

static const char *TAG = "button";

static void button_event_cb(void *arg, void *data)
{
    //iot_button_print_event((button_handle_t)arg);
   button_event_t event = (button_event_t)data;

    switch (event) {
        case BUTTON_PRESS_DOWN:
            // 处理按钮按下事件
            break;
        case BUTTON_PRESS_UP:
            // 处理按钮释放事件
            break;
        case BUTTON_SINGLE_CLICK:
            // 处理单击事件
            buzzer(NOTE_C4, 7168, 1, 1, 1); // 蜂鸣器发出音符C4
            break;
        case BUTTON_DOUBLE_CLICK:
            // 处理双击事件
            buzzer(NOTE_E4, 7168, 1, 1, 1); // 蜂鸣器发出音符E4
            break;
        case BUTTON_LONG_PRESS_START:
            //处理长按事件
            break;

        // 其他事件处理...
        default:
            break;
    }
}

#if CONFIG_ENTER_LIGHT_SLEEP_MODE_MANUALLY
void button_enter_power_save(void *usr_data)
{
    ESP_LOGI(TAG, "Can enter power save now");
    esp_light_sleep_start();
}
#endif

void button_init(uint32_t button_num)
{
    button_config_t btn_cfg = {0};
    button_gpio_config_t gpio_cfg = {
        .gpio_num = button_num,
        .active_level = BUTTON_ACTIVE_LEVEL,
        .enable_power_save = true,
    };

    button_handle_t btn;
    esp_err_t ret = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn);
    assert(ret == ESP_OK);

    ret = iot_button_register_cb(btn, BUTTON_PRESS_DOWN, NULL, button_event_cb, NULL);
    ret |= iot_button_register_cb(btn, BUTTON_PRESS_UP, NULL, button_event_cb, NULL);
    ret |= iot_button_register_cb(btn, BUTTON_PRESS_REPEAT, NULL, button_event_cb, NULL);
    ret |= iot_button_register_cb(btn, BUTTON_PRESS_REPEAT_DONE, NULL, button_event_cb, NULL);
    ret |= iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, button_event_cb, NULL);
    ret |= iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL, button_event_cb, NULL);
    ret |= iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, button_event_cb, NULL);
    ret |= iot_button_register_cb(btn, BUTTON_LONG_PRESS_HOLD, NULL, button_event_cb, NULL);
    ret |= iot_button_register_cb(btn, BUTTON_LONG_PRESS_UP, NULL, button_event_cb, NULL);
    ret |= iot_button_register_cb(btn, BUTTON_PRESS_END, NULL, button_event_cb, NULL);

#if CONFIG_ENTER_LIGHT_SLEEP_MODE_MANUALLY
    /*!< For enter Power Save */
    button_power_save_config_t config = {
        .enter_power_save_cb = button_enter_power_save,
    };
    ret |= iot_button_register_power_save_cb(&config);
#endif

    ESP_ERROR_CHECK(ret);
}