/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "button_gpio.h"
#include "iot_button.h"

#include "espaperplay_config.h"
#include "espaperplay_input.h"

static const char *TAG = "ESPaperPlay_INPUT";

/**
 * 事件队列设计：按键与触摸物理隔离为两个队列，经 FreeRTOS Queue Set
 * 合并消费（espaperplay_input_get_event() 阻塞等待任一队列）——
 *
 *   - 按键队列（16 深）：稀疏、事件型（单击/双击/长按等语义动作），
 *     投递到队首（xQueueSendToFront），满时挤掉最旧，按键永不丢失；
 *   - 触摸队列（32 深）：高频、状态型（中断触发读取的坐标流），满时
 *     丢旧保新（挤掉最旧坐标，队列永远保留最新触摸状态），触摸洪泛
 *     不会挤占按键队列。
 *
 * 两个队列互不干扰：长按 HOLD 节流（500ms）后按键速率 ~2/秒，触摸
 * 即使 300 事件/秒也只会触发触摸队列自身的丢旧保新。
 */
#define ESPAPERPLAY_INPUT_KEY_QUEUE_LEN 16   /*!< 按键事件队列长度（条） */
#define ESPAPERPLAY_INPUT_TOUCH_QUEUE_LEN 32 /*!< 触摸事件队列长度（条） */

/**
 * LONG_PRESS_HOLD 事件的最小投递间隔（毫秒）。
 *
 * 驱动默认每 CONFIG_BUTTON_LONG_PRESS_HOLD_SERIAL_TIME_MS（20ms）触发一次
 * HOLD。20ms 粒度的 HOLD 对 e-paper UI 毫无意义，反而会形成事件洪泛，
 * 导致长按松开（LONG_PRESS_UP）漏检。按本间隔节流（约 2 个/秒），既保留
 * "长按进行中"的周期反馈，又不淹没关键事件。
 */
#define ESPAPERPLAY_INPUT_HOLD_MIN_INTERVAL_MS 500

static QueueHandle_t s_key_queue;    /*!< 按键事件队列 */
static QueueHandle_t s_touch_queue;  /*!< 触摸事件队列 */
static QueueSetHandle_t s_queue_set; /*!< 双队列合并等待（容量 = 两队列深度之和） */
static button_handle_t s_boot_button;
static uint32_t s_last_hold_ms = 0; /*!< 上次投递 HOLD 的时刻（esp_timer 毫秒） */

/**
 * @brief 将按键动作投递到按键队列。
 *
 * 运行在 espressif/button 驱动的扫描任务上下文（esp_timer ESP_TIMER_TASK）中：
 * 必须快速返回，禁止阻塞操作（队列操作均使用 0 超时）。
 *
 * 按键投递到队首（xQueueSendToFront），队列满时挤掉最旧的排队按键事件，
 * 保证最新按键状态优先被消费（与触摸队列完全隔离，互不挤占）。
 *
 * @param key_id      按键标识。
 * @param key_action  归一化按键动作。
 * @param press_time  本次按压持续时间（毫秒）。
 */
static void input_post_key_event(uint8_t key_id, espaperplay_input_key_action_t key_action,
                                 uint16_t press_time) {
    if (s_key_queue == NULL) {
        return;
    }

    const espaperplay_input_event_t event = {
        .type = ESPAPERPLAY_INPUT_EVENT_KEY,
        .key_id = key_id,
        .key_action = key_action,
        .key_press_time_ms = press_time,
    };

    if (xQueueSendToFront(s_key_queue, &event, 0) != pdTRUE) {
        /* 队列满：丢弃最旧的排队按键事件后重试（按键不丢失）。 */
        espaperplay_input_event_t dropped;
        if (xQueueReceive(s_key_queue, &dropped, 0) == pdTRUE) {
            (void)xQueueSendToFront(s_key_queue, &event, 0);
        } else {
            ESP_LOGW(TAG, "key event queue full, key event dropped");
        }
    }
}

/**
 * @brief espressif/button 通用回调。
 *
 * @param button_handle 触发事件的按键句柄（由驱动传入）。
 * @param usr_data      用户数据（未使用）。
 */
static void boot_button_event_cb(void *button_handle, void *usr_data) {
    (void)usr_data;

    const button_event_t btn_event = iot_button_get_event(button_handle);
    const uint16_t press_time = (uint16_t)iot_button_get_pressed_time(button_handle);

    switch (btn_event) {
    case BUTTON_PRESS_DOWN:
        input_post_key_event(ESPAPERPLAY_INPUT_KEY_ID_BOOT, ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_DOWN,
                             press_time);
        break;
    case BUTTON_PRESS_UP:
        input_post_key_event(ESPAPERPLAY_INPUT_KEY_ID_BOOT, ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_UP,
                             press_time);
        break;
    case BUTTON_SINGLE_CLICK:
        input_post_key_event(ESPAPERPLAY_INPUT_KEY_ID_BOOT,
                             ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK, press_time);
        break;
    case BUTTON_DOUBLE_CLICK:
        input_post_key_event(ESPAPERPLAY_INPUT_KEY_ID_BOOT,
                             ESPAPERPLAY_INPUT_KEY_ACTION_DOUBLE_CLICK, press_time);
        break;
    case BUTTON_LONG_PRESS_START:
        input_post_key_event(ESPAPERPLAY_INPUT_KEY_ID_BOOT,
                             ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_START, press_time);
        break;
    case BUTTON_LONG_PRESS_HOLD: {
        /* 节流：丢弃过密的 HOLD（驱动每 20ms 触发一次），防止事件洪泛
         * 淹没 LONG_PRESS_UP 等关键事件。无符号减法天然处理回绕。 */
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - s_last_hold_ms < ESPAPERPLAY_INPUT_HOLD_MIN_INTERVAL_MS) {
            return;
        }
        s_last_hold_ms = now_ms;
        input_post_key_event(ESPAPERPLAY_INPUT_KEY_ID_BOOT,
                             ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_HOLD, press_time);
        break;
    }
    case BUTTON_LONG_PRESS_UP:
        input_post_key_event(ESPAPERPLAY_INPUT_KEY_ID_BOOT,
                             ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_UP, press_time);
        break;
    default:
        /* 未归一化的事件（PRESS_REPEAT / MULTIPLE_CLICK / PRESS_END 等）不投递。 */
        break;
    }
}

/**
 * @brief 为按键注册指定事件的回调。
 */
static esp_err_t register_button_event(button_handle_t button, button_event_t event) {
    const esp_err_t ret = iot_button_register_cb(button, event, NULL, boot_button_event_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register callback for event %s failed: %s", iot_button_get_event_str(event),
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t espaperplay_input_init(void) {
    /* 双队列 + Queue Set（容量必须 >= 成员队列深度之和，否则投递会失败）。 */
    s_key_queue = xQueueCreate(ESPAPERPLAY_INPUT_KEY_QUEUE_LEN, sizeof(espaperplay_input_event_t));
    s_touch_queue =
        xQueueCreate(ESPAPERPLAY_INPUT_TOUCH_QUEUE_LEN, sizeof(espaperplay_input_event_t));
    s_queue_set =
        xQueueCreateSet(ESPAPERPLAY_INPUT_KEY_QUEUE_LEN + ESPAPERPLAY_INPUT_TOUCH_QUEUE_LEN);
    if (s_key_queue == NULL || s_touch_queue == NULL || s_queue_set == NULL) {
        ESP_LOGE(TAG, "failed to create input queues / queue set");
        if (s_key_queue != NULL) {
            vQueueDelete(s_key_queue);
        }
        if (s_touch_queue != NULL) {
            vQueueDelete(s_touch_queue);
        }
        if (s_queue_set != NULL) {
            vQueueDelete(s_queue_set);
        }
        s_key_queue = s_touch_queue = s_queue_set = NULL;
        return ESP_ERR_NO_MEM;
    }
    xQueueAddToSet(s_key_queue, s_queue_set);
    xQueueAddToSet(s_touch_queue, s_queue_set);

    /* BOOT 按键：GPIO0，按下为低电平，使能内部上拉（disable_pull = false）。
     * enable_power_save 暂不开启：电源组件（浅睡眠唤醒）接入后再启用。 */
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = ESPAPERPLAY_PIN_KEY_BOOT,
        .active_level = ESPAPERPLAY_KEY_BOOT_ACTIVE_LEVEL,
        .enable_power_save = false,
        .disable_pull = false,
    };

    esp_err_t ret = iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &s_boot_button);
    if (ret != ESP_OK || s_boot_button == NULL) {
        ESP_LOGE(TAG, "failed to create BOOT button on GPIO%d (ret: %s)", ESPAPERPLAY_PIN_KEY_BOOT,
                 esp_err_to_name(ret));
        vQueueDelete(s_queue_set);
        vQueueDelete(s_touch_queue);
        vQueueDelete(s_key_queue);
        s_queue_set = s_touch_queue = s_key_queue = NULL;
        return (ret != ESP_OK) ? ret : ESP_FAIL;
    }

    /* 归一化并投递的按键事件集合。 */
    const button_event_t events[] = {
        BUTTON_PRESS_DOWN,       BUTTON_PRESS_UP,        BUTTON_SINGLE_CLICK,  BUTTON_DOUBLE_CLICK,
        BUTTON_LONG_PRESS_START, BUTTON_LONG_PRESS_HOLD, BUTTON_LONG_PRESS_UP,
    };
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        ret = register_button_event(s_boot_button, events[i]);
        if (ret != ESP_OK) {
            iot_button_delete(s_boot_button);
            s_boot_button = NULL;
            vQueueDelete(s_queue_set);
            vQueueDelete(s_touch_queue);
            vQueueDelete(s_key_queue);
            s_queue_set = s_touch_queue = s_key_queue = NULL;
            return ret;
        }
    }

    ESP_LOGI(TAG,
             "Input subsystem ready: BOOT key on GPIO%d (active level %d), "
             "key queue %d + touch queue %d via queue set",
             ESPAPERPLAY_PIN_KEY_BOOT, ESPAPERPLAY_KEY_BOOT_ACTIVE_LEVEL,
             ESPAPERPLAY_INPUT_KEY_QUEUE_LEN, ESPAPERPLAY_INPUT_TOUCH_QUEUE_LEN);
    return ESP_OK;
}

esp_err_t espaperplay_input_get_event(espaperplay_input_event_t *event, uint32_t timeout_ms) {
    if (event == NULL || s_queue_set == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 按键优先：非阻塞先查按键队列（按键稀疏、延迟敏感；触摸事件稍候）。 */
    if (xQueueReceive(s_key_queue, event, 0) == pdTRUE) {
        return ESP_OK;
    }

    /* 阻塞等待任一队列（Queue Set 合并：按键 / 触摸）。 */
    const TickType_t ticks =
        (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    const QueueSetMemberHandle_t member = xQueueSelectFromSet(s_queue_set, ticks);
    if (member == NULL) {
        return ESP_ERR_TIMEOUT;
    }
    if (xQueueReceive(member, event, 0) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT; /* select 返回非空后理论上必可取到 */
}

const char *espaperplay_input_key_action_str(espaperplay_input_key_action_t action) {
    switch (action) {
    case ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_DOWN:
        return "PRESS_DOWN";
    case ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_UP:
        return "PRESS_UP";
    case ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK:
        return "SINGLE_CLICK";
    case ESPAPERPLAY_INPUT_KEY_ACTION_DOUBLE_CLICK:
        return "DOUBLE_CLICK";
    case ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_START:
        return "LONG_PRESS_START";
    case ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_HOLD:
        return "LONG_PRESS_HOLD";
    case ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_UP:
        return "LONG_PRESS_UP";
    default:
        return "UNKNOWN";
    }
}

esp_err_t espaperplay_input_post_event(const espaperplay_input_event_t *event) {
    if (event == NULL || event->type >= ESPAPERPLAY_INPUT_EVENT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_key_queue == NULL || s_touch_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (event->type == ESPAPERPLAY_INPUT_EVENT_KEY) {
        /* 按键：队首 + 满时挤最旧（与按键回调投递策略一致）。 */
        if (xQueueSendToFront(s_key_queue, event, 0) != pdTRUE) {
            espaperplay_input_event_t dropped;
            if (xQueueReceive(s_key_queue, &dropped, 0) == pdTRUE) {
                (void)xQueueSendToFront(s_key_queue, event, 0);
            } else {
                ESP_LOGW(TAG, "key event queue full, injected key event dropped");
            }
        }
    } else {
        /* 触摸：满时丢旧保新——队列永远保留最新触摸状态。 */
        if (xQueueSend(s_touch_queue, event, 0) != pdTRUE) {
            espaperplay_input_event_t dropped;
            if (xQueueReceive(s_touch_queue, &dropped, 0) == pdTRUE) {
                (void)xQueueSend(s_touch_queue, event, 0);
            } else {
                ESP_LOGW(TAG, "touch event queue full, injected touch event dropped");
            }
        }
    }
    return ESP_OK;
}
