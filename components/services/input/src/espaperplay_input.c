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
#include "espaperplay_system.h"

static const char *TAG = "ESPaperPlay_INPUT";

/**
 * 事件队列设计：按键与触摸物理隔离为两个独立队列，由 LVGL 线程分别
 * 直读（按键经按键泵 lv_timer 排空、触摸经 indev read_cb 排空，见 ui
 * 组件）——
 *
 *   - 按键队列（16 深）：稀疏、事件型（单击/双击/长按等语义动作），
 *     投递到队首（xQueueSendToFront），满时挤掉最旧，按键永不丢失；
 *   - 触摸队列（32 深）：高频、状态型（中断触发读取的坐标流，轨迹绘制
 *     需要中间点，不能只留最新一帧）。满时丢弃新事件，触摸洪泛不阻塞
 *     读取任务；LVGL 线程每次 read 周期（~30ms）全量排空，积压有限。
 *
 * 两个队列互不干扰：长按 HOLD 节流（500ms）后按键速率 ~2/秒。
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

static QueueHandle_t s_key_queue;   /*!< 按键事件队列 */
static QueueHandle_t s_touch_queue; /*!< 触摸事件队列 */

/** 板载按键表：key_id -> 驱动句柄（init 时按序创建）。 */
#define ESPAPERPLAY_INPUT_BUTTON_COUNT 3
static button_handle_t s_buttons[ESPAPERPLAY_INPUT_BUTTON_COUNT];
static uint32_t s_last_hold_ms[ESPAPERPLAY_INPUT_BUTTON_COUNT]; /*!< 上次投递 HOLD 的时刻（esp_timer 毫秒） */

static uint16_t s_touch_seq = 0; /*!< 触摸帧序号（每帧递增，同帧各点共享） */

/* 最近一次用户活动时刻（esp_timer 毫秒）。按键 / 触摸事件投递时刷新，
 * 供电源管理判断"无操作"超时。非原子读写（仅用于超时比较，误差可忽略）。 */
static uint64_t s_last_activity_ms = 0;

/* "设备已进入睡眠"指示标志：电源管理在即将睡眠前置位、用户唤醒后清除。
 * UI 各页面据此在状态栏显示节能图标。存于 input 组件以避免 power↔ui
 * REQUIRES 环（ui 已 REQUIRES power）。 */
static bool s_sleep_indicator = false;

/** 记录一次用户活动（刷新活动时间戳）。 */
static inline void input_mark_activity(void) {
    s_last_activity_ms = (uint64_t)(esp_timer_get_time() / 1000);
}

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

    /* 任意按键动作均视为用户活动（含按下 / 松开 / 长按保持等）。 */
    input_mark_activity();

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
 * @brief GT911 帧回调：把触摸帧归一化为输入事件投递到触摸队列。
 *
 * 由 touch 驱动的读取任务上下文调用（非 ISR）：按下帧每个触摸点投递
 * 一个事件（touch_seq 相同的点属于同一帧，touch_points 记录本帧点数），
 * 释放帧（count == 0）投递一个 touch_pressed = 0 的事件。队列满时由
 * espaperplay_input_post_event() 丢旧保新，触摸洪泛不阻塞读取任务。
 *
 * @param points 当前帧触摸点数组（count == 0 时无效）。
 * @param count  当前帧触摸点数量（0 = 全部手指抬起）。
 */
static void input_touch_event_cb(const espaperplay_touch_point_t *points, uint8_t count) {
    const uint16_t seq = ++s_touch_seq;

    /* 任意触摸帧（按下 / 抬起）均视为用户活动。 */
    input_mark_activity();

    if (count == 0) {
        const espaperplay_input_event_t event = {
            .type = ESPAPERPLAY_INPUT_EVENT_TOUCH,
            .touch_pressed = 0,
            .touch_points = 0,
            .touch_seq = seq,
        };
        (void)espaperplay_input_post_event(&event);
        return;
    }

    for (uint8_t i = 0; i < count; i++) {
        const espaperplay_input_event_t event = {
            .type = ESPAPERPLAY_INPUT_EVENT_TOUCH,
            .point = points[i],
            .touch_pressed = 1,
            .touch_points = count,
            .touch_seq = seq,
        };
        (void)espaperplay_input_post_event(&event);
    }
}

/**
 * @brief espressif/button 通用回调（三键共用，usr_data 携带 key_id）。
 *
 * @param button_handle 触发事件的按键句柄（由驱动传入）。
 * @param usr_data      按键标识（intptr_t 包装的 key_id）。
 */
static void button_event_cb(void *button_handle, void *usr_data) {
    const uint8_t key_id = (uint8_t)(intptr_t)usr_data;

    const button_event_t btn_event = iot_button_get_event(button_handle);
    const uint16_t press_time = (uint16_t)iot_button_get_pressed_time(button_handle);

    switch (btn_event) {
    case BUTTON_PRESS_DOWN:
        input_post_key_event(key_id, ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_DOWN, press_time);
        break;
    case BUTTON_PRESS_UP:
        input_post_key_event(key_id, ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_UP, press_time);
        break;
    case BUTTON_SINGLE_CLICK:
        input_post_key_event(key_id, ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK, press_time);
        break;
    case BUTTON_DOUBLE_CLICK:
        input_post_key_event(key_id, ESPAPERPLAY_INPUT_KEY_ACTION_DOUBLE_CLICK, press_time);
        break;
    case BUTTON_LONG_PRESS_START:
        input_post_key_event(key_id, ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_START, press_time);
        break;
    case BUTTON_LONG_PRESS_HOLD: {
        /* 节流：丢弃过密的 HOLD（驱动每 20ms 触发一次），防止事件洪泛
         * 淹没 LONG_PRESS_UP 等关键事件。无符号减法天然处理回绕。 */
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - s_last_hold_ms[key_id] < ESPAPERPLAY_INPUT_HOLD_MIN_INTERVAL_MS) {
            return;
        }
        s_last_hold_ms[key_id] = now_ms;
        input_post_key_event(key_id, ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_HOLD, press_time);
        break;
    }
    case BUTTON_LONG_PRESS_UP:
        input_post_key_event(key_id, ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_UP, press_time);
        break;
    default:
        /* 未归一化的事件（PRESS_REPEAT / MULTIPLE_CLICK / PRESS_END 等）不投递。 */
        break;
    }
}

/** 单个按键的静态配置（引脚 / 有效电平 / key_id）。 */
typedef struct {
    uint8_t key_id;
    int gpio_num;
    int active_level;
    uint16_t long_press_time_ms;
} input_button_desc_t;

esp_err_t espaperplay_input_init(void) {
    /* 双队列（按键 / 触摸物理隔离，LVGL 线程分别直读）。 */
    s_key_queue = xQueueCreate(ESPAPERPLAY_INPUT_KEY_QUEUE_LEN, sizeof(espaperplay_input_event_t));
    s_touch_queue =
        xQueueCreate(ESPAPERPLAY_INPUT_TOUCH_QUEUE_LEN, sizeof(espaperplay_input_event_t));
    if (s_key_queue == NULL || s_touch_queue == NULL) {
        ESP_LOGE(TAG, "failed to create input queues");
        if (s_key_queue != NULL) {
            vQueueDelete(s_key_queue);
        }
        if (s_touch_queue != NULL) {
            vQueueDelete(s_touch_queue);
        }
        s_key_queue = s_touch_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 触摸源接入：GT911 帧回调 -> 触摸队列（须在 espaperplay_touch_init()
     * 之后调用本函数）。touch 驱动未就绪时仅告警，按键输入不受影响。 */
    const esp_err_t touch_cb_ret = espaperplay_touch_register_event_cb(input_touch_event_cb);
    if (touch_cb_ret != ESP_OK) {
        ESP_LOGW(TAG, "register touch event callback failed (%s), touch queue inactive",
                 esp_err_to_name(touch_cb_ret));
    }

    /* 板载三键：BOOT（GPIO0，内部上拉）/ VOL+（GPIO2，外部上拉）/
     * VOL-（GPIO5，外部上拉），均按下为低电平。
     * enable_power_save 开启：使按键 GPIO 在浅睡眠期间保持可唤醒配置，
     * 配合电源组件的 GPIO 唤醒源（低电平）实现按键唤醒。
     * BOOT 长按判定时间取系统配置（Web 可配置，NVS 持久化，默认 1000ms），
     * 覆盖驱动编译期默认值（CONFIG_BUTTON_LONG_PRESS_TIME_MS=1500）；
     * 音量键保持驱动默认（1500ms），仅单击 / 长按保持参与音量调节。 */
    const input_button_desc_t button_descs[ESPAPERPLAY_INPUT_BUTTON_COUNT] = {
        {
            .key_id = ESPAPERPLAY_INPUT_KEY_ID_BOOT,
            .gpio_num = ESPAPERPLAY_PIN_KEY_BOOT,
            .active_level = ESPAPERPLAY_KEY_BOOT_ACTIVE_LEVEL,
            .long_press_time_ms =
                (uint16_t)espaperplay_system_get_config()->boot_long_press_time_ms,
        },
        {
            .key_id = ESPAPERPLAY_INPUT_KEY_ID_VOL_UP,
            .gpio_num = ESPAPERPLAY_PIN_KEY_VOL_UP,
            .active_level = ESPAPERPLAY_KEY_VOL_UP_ACTIVE_LEVEL,
            .long_press_time_ms = 0, /* 0=沿用驱动默认 */
        },
        {
            .key_id = ESPAPERPLAY_INPUT_KEY_ID_VOL_DOWN,
            .gpio_num = ESPAPERPLAY_PIN_KEY_VOL_DOWN,
            .active_level = ESPAPERPLAY_KEY_VOL_DOWN_ACTIVE_LEVEL,
            .long_press_time_ms = 0,
        },
    };

    const button_event_t events[] = {
        BUTTON_PRESS_DOWN,       BUTTON_PRESS_UP,        BUTTON_SINGLE_CLICK,  BUTTON_DOUBLE_CLICK,
        BUTTON_LONG_PRESS_START, BUTTON_LONG_PRESS_HOLD, BUTTON_LONG_PRESS_UP,
    };

    for (size_t b = 0; b < ESPAPERPLAY_INPUT_BUTTON_COUNT; b++) {
        const input_button_desc_t *desc = &button_descs[b];

        button_config_t btn_cfg = { 0 };
        if (desc->long_press_time_ms > 0) {
            btn_cfg.long_press_time = desc->long_press_time_ms;
        }
        const button_gpio_config_t btn_gpio_cfg = {
            .gpio_num = desc->gpio_num,
            .active_level = desc->active_level,
            .enable_power_save = true,
            .disable_pull = false,
        };

        esp_err_t ret =
            iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &s_buttons[b]);
        if (ret != ESP_OK || s_buttons[b] == NULL) {
            ESP_LOGE(TAG, "failed to create button key%u on GPIO%d (ret: %s)", desc->key_id,
                     desc->gpio_num, esp_err_to_name(ret));
            for (size_t j = 0; j < b; j++) {
                iot_button_delete(s_buttons[j]);
                s_buttons[j] = NULL;
            }
            vQueueDelete(s_touch_queue);
            vQueueDelete(s_key_queue);
            s_touch_queue = s_key_queue = NULL;
            return (ret != ESP_OK) ? ret : ESP_FAIL;
        }

        for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
            ret = iot_button_register_cb(s_buttons[b], events[i], NULL, button_event_cb,
                                         (void *)(intptr_t)desc->key_id);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "register event %s for key%u failed: %s",
                         iot_button_get_event_str(events[i]), desc->key_id,
                         esp_err_to_name(ret));
                for (size_t j = 0; j <= b; j++) {
                    iot_button_delete(s_buttons[j]);
                    s_buttons[j] = NULL;
                }
                vQueueDelete(s_touch_queue);
                vQueueDelete(s_key_queue);
                s_touch_queue = s_key_queue = NULL;
                return ret;
            }
        }
    }

    ESP_LOGI(TAG,
             "Input subsystem ready: BOOT(GPIO%d) + VOL+(GPIO%d) + VOL-(GPIO%d), "
             "key queue %d + touch queue %d (read by LVGL thread)",
             ESPAPERPLAY_PIN_KEY_BOOT, ESPAPERPLAY_PIN_KEY_VOL_UP,
             ESPAPERPLAY_PIN_KEY_VOL_DOWN, ESPAPERPLAY_INPUT_KEY_QUEUE_LEN,
             ESPAPERPLAY_INPUT_TOUCH_QUEUE_LEN);
    return ESP_OK;
}

esp_err_t espaperplay_input_try_get_key(espaperplay_input_event_t *event) {
    if (event == NULL || s_key_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueReceive(s_key_queue, event, 0) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t espaperplay_input_try_get_touch(espaperplay_input_event_t *event) {
    if (event == NULL || s_touch_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueReceive(s_touch_queue, event, 0) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

uint64_t espaperplay_input_get_last_activity_ms(void) { return s_last_activity_ms; }

void espaperplay_input_mark_activity(void) { input_mark_activity(); }

void espaperplay_input_set_sleep_indicator(bool shown) { s_sleep_indicator = shown; }

bool espaperplay_input_is_sleep_indicator(void) { return s_sleep_indicator; }

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
        /* 触摸：普通投递；满时丢弃新事件（轨迹中间点，短暂过载只丢最新，
         * 顺序不乱）。LVGL 线程每个 read 周期全量排空，正常不积压。 */
        static uint32_t s_touch_drop_log_ms = 0;
        if (xQueueSend(s_touch_queue, event, 0) != pdTRUE) {
            const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - s_touch_drop_log_ms >= 1000) {
                s_touch_drop_log_ms = now_ms;
                ESP_LOGW(TAG, "touch queue full, dropping newest touch event");
            }
        }
    }
    return ESP_OK;
}

esp_err_t espaperplay_input_set_boot_long_press_time_ms(uint32_t time_ms) {
    if (time_ms < ESPAPERPLAY_INPUT_LONG_PRESS_TIME_MIN_MS ||
        time_ms > ESPAPERPLAY_INPUT_LONG_PRESS_TIME_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    const button_handle_t boot_button = s_buttons[ESPAPERPLAY_INPUT_KEY_ID_BOOT];
    if (boot_button == NULL) {
        ESP_LOGW(TAG, "BOOT button not initialized yet, long-press time applied at init");
        return ESP_ERR_INVALID_STATE;
    }
    /* 驱动以"值即毫秒"语义接收（内部除以扫描周期得到 tick 数）。 */
    const esp_err_t ret =
        iot_button_set_param(boot_button, BUTTON_LONG_PRESS_TIME_MS, (void *)(intptr_t)time_ms);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "BOOT long-press time set to %u ms", (unsigned)time_ms);
    } else {
        ESP_LOGE(TAG, "BOOT long-press time set failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
