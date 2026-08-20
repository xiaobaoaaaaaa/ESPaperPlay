/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "espaperplay_gui_lv.h"
#include "espaperplay_input.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"

static const char *TAG = "ESPaperPlay_UI";

#define ESPAPERPLAY_UI_KEY_INPUT_TASK_STACK_SIZE 4096
#define ESPAPERPLAY_UI_KEY_INPUT_TASK_PRIORITY 4

/** 触摸批量投递：窗口（毫秒）内累积的事件一并转发到 LVGL 线程。 */
#define UI_TOUCH_BATCH_WINDOW_MS 30
/** 触摸批量投递：单批最大事件数（触发立即转发，防止窗口内超量）。 */
#define UI_TOUCH_BATCH_MAX_EVENTS 32

/* ====================================================================
 * 输入分发：input 事件队列 -> LVGL 线程 -> 页面钩子 / 触摸指针
 * ====================================================================
 *
 * 输入分发任务阻塞在 espaperplay_input_get_event() 上（按键、触摸事件
 * 走同一条合并队列）。分发策略：
 *   - 按键：事件型（低速率），每次经 espaperplay_gui_lv_call 投递到
 *     LVGL 线程，由页面栈转发给栈顶页面 on_key（导航/刷新内容）；
 *   - 触摸：高频状态型（GT911 上报可达 ~100 帧/秒）。指针 indev 状态
 *     直接在本任务逐事件更新（espaperplay_ui_touch_update，临界区保护，
 *     无 LVGL 往返延迟）；页面级展示（轨迹绘制需要每个中间点）按
 *     UI_TOUCH_BATCH_WINDOW_MS 窗口批量投递一次 gui_lv_call，批内事件
 *     逐条转发给页面 on_touch——既保留全部中间坐标，又摊薄跨线程开销
 *     （批内点在同一 LVGL 周期渲染，脏区自然合并为一次 e-paper 刷新）。
 */

/** LVGL 线程内执行：把按键事件转发给栈顶页面（arg 指向任务栈上的事件副本）。 */
static void ui_key_dispatch_cb(void *arg) {
    const espaperplay_input_event_t *event = (const espaperplay_input_event_t *)arg;

    ESP_LOGI(TAG, "key event: id=%u action=%s press=%u ms", event->key_id,
             espaperplay_input_key_action_str(event->key_action), event->key_press_time_ms);

    espaperplay_ui_page_handle_key_lv(event);
}

/** 触摸批量投递描述（分发任务栈上的事件数组 + 数量）。 */
typedef struct {
    const espaperplay_input_event_t *events; /*!< 本批事件数组 */
    uint16_t count;                          /*!< 本批事件数 */
} ui_touch_batch_t;

/** LVGL 线程内执行：把批量触摸事件逐条转发给栈顶页面（on_touch）。 */
static void ui_touch_batch_dispatch_cb(void *arg) {
    const ui_touch_batch_t *batch = (const ui_touch_batch_t *)arg;
    for (uint16_t i = 0; i < batch->count; i++) {
        espaperplay_ui_page_handle_touch_lv(&batch->events[i]);
    }
}

#if ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST
#define ESPAPERPLAY_UI_SELFTEST_TASK_STACK_SIZE 3072
#define ESPAPERPLAY_UI_SELFTEST_TASK_PRIORITY 3
#define ESPAPERPLAY_UI_SELFTEST_WAIT_MS 3000

/** 等待页面栈深度达到期望值（自检用）。 */
static bool ui_key_selftest_wait_depth(uint8_t want, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (espaperplay_ui_page_depth() != want && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    return espaperplay_ui_page_depth() == want;
}

/**
 * 按键链路自检任务：注入合成按键事件，断言 GUI 的读取与处理。
 *
 * 独立任务运行，按键分发任务在此期间持续消费输入队列——注入事件与真实
 * 按键走完全相同的路径（input 队列 -> 分发任务 -> LVGL 线程 -> 页面钩子）。
 *
 * 用例 1：注入 SINGLE_CLICK，期望 home 的 on_key 压入测试页
 *         （页面栈深度 1 -> 2）；
 * 用例 2：注入 LONG_PRESS_UP，期望 测试页的 on_key 弹出返回 home
 *         （深度 2 -> 1，验证 pop 重建上一页）；
 * 用例 3：再次注入 SINGLE_CLICK，期望深度 1 -> 2（往返后系统仍存活、
 *         页面栈与分发链路工作正常）。
 */
static void ui_key_selftest_task(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "key selftest start (injecting synthetic key events)");
    vTaskDelay(pdMS_TO_TICKS(200)); /* 等待分发任务进入事件循环 */

    uint32_t passed = 0;
    uint32_t failed = 0;

    espaperplay_input_event_t event = {
        .type = ESPAPERPLAY_INPUT_EVENT_KEY,
        .key_id = ESPAPERPLAY_INPUT_KEY_ID_BOOT,
        .key_action = ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK,
        .key_press_time_ms = 120,
    };
    if (espaperplay_input_post_event(&event) != ESP_OK) {
        ESP_LOGE(TAG, "key selftest: post SINGLE_CLICK failed");
        failed++;
    } else if (!ui_key_selftest_wait_depth(2, ESPAPERPLAY_UI_SELFTEST_WAIT_MS)) {
        ESP_LOGE(TAG,
                 "key selftest: SINGLE_CLICK did not push test page "
                 "(depth=%u, want 2)",
                 (unsigned)espaperplay_ui_page_depth());
        failed++;
    } else {
        passed++;
    }

    event.key_action = ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_UP;
    event.key_press_time_ms = 1600;
    if (espaperplay_input_post_event(&event) != ESP_OK) {
        ESP_LOGE(TAG, "key selftest: post LONG_PRESS_UP failed");
        failed++;
    } else if (!ui_key_selftest_wait_depth(1, ESPAPERPLAY_UI_SELFTEST_WAIT_MS)) {
        ESP_LOGE(TAG,
                 "key selftest: LONG_PRESS_UP did not pop back to home "
                 "(depth=%u, want 1)",
                 (unsigned)espaperplay_ui_page_depth());
        failed++;
    } else {
        passed++;
    }

    event.key_action = ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK;
    event.key_press_time_ms = 120;
    if (espaperplay_input_post_event(&event) != ESP_OK) {
        ESP_LOGE(TAG, "key selftest: post 2nd SINGLE_CLICK failed");
        failed++;
    } else if (!ui_key_selftest_wait_depth(2, ESPAPERPLAY_UI_SELFTEST_WAIT_MS)) {
        ESP_LOGE(TAG,
                 "key selftest: 2nd SINGLE_CLICK did not push test page "
                 "(depth=%u, want 2)",
                 (unsigned)espaperplay_ui_page_depth());
        failed++;
    } else {
        passed++;
    }

    ESP_LOGI(TAG, "key selftest %s (%u/%u passed)", (failed == 0) ? "PASS" : "FAIL",
             (unsigned)passed, (unsigned)(passed + failed));

    /* 收尾：返回 home，保持系统处于初始状态。 */
    event.key_action = ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_UP;
    (void)espaperplay_input_post_event(&event);

    vTaskDelete(NULL);
}
#endif /* ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST */

/** 按键分发任务：阻塞读取输入队列，按键/触摸事件分路处理。 */
static void ui_key_input_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "key input task started");

    /* 触摸批量投递缓冲（本任务独占；gui_lv_call 阻塞等待执行完成，
     * 批数组在回调执行期间保持有效）。 */
    espaperplay_input_event_t touch_batch[UI_TOUCH_BATCH_MAX_EVENTS];
    uint16_t touch_batch_count = 0;
    uint32_t touch_batch_start_ms = 0;

    for (;;) {
        espaperplay_input_event_t event;
        const esp_err_t err = espaperplay_input_get_event(&event, portMAX_DELAY);
        if (err != ESP_OK) {
            /* 输入子系统未初始化等异常：避免忙等空转。 */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (event.type == ESPAPERPLAY_INPUT_EVENT_TOUCH) {
            /* 指针 indev 状态：逐事件直接更新（临界区保护），无 LVGL
             * 往返延迟。 */
            espaperplay_ui_touch_update(&event);

            /* 页面展示：窗口内累积批量投递（保留全部中间点）；释放事件
             * 必须立即转发（笔画收尾）。gui_lv_call 的完成信号是全局单
             * 标志（单调用方语义），不可跳过等待，否则本次完成信号会
             * 污染后续事件的完成同步。 */
            const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (touch_batch_count == 0) {
                touch_batch_start_ms = now_ms;
            }
            touch_batch[touch_batch_count++] = event;

            const bool batch_full = touch_batch_count >= UI_TOUCH_BATCH_MAX_EVENTS;
            const bool window_elapsed = now_ms - touch_batch_start_ms >= UI_TOUCH_BATCH_WINDOW_MS;
            if (batch_full || window_elapsed || event.touch_pressed == 0) {
                ui_touch_batch_t batch = {touch_batch, touch_batch_count};
                const esp_err_t call_err =
                    espaperplay_gui_lv_call(ui_touch_batch_dispatch_cb, &batch, 1000);
                if (call_err != ESP_OK) {
                    ESP_LOGW(TAG, "dispatch touch batch to LVGL failed: %s",
                             esp_err_to_name(call_err));
                }
                touch_batch_count = 0;
            }
            continue;
        }

        /* 按键事件：同步投递到 LVGL 线程处理。 */
        const esp_err_t call_err = espaperplay_gui_lv_call(ui_key_dispatch_cb, &event, 1000);
        if (call_err != ESP_OK) {
            ESP_LOGW(TAG, "dispatch key event to LVGL failed: %s", esp_err_to_name(call_err));
        }
    }
}

esp_err_t espaperplay_ui_key_input_start(void) {
    if (xTaskCreate(ui_key_input_task, "ui_key_input", ESPAPERPLAY_UI_KEY_INPUT_TASK_STACK_SIZE,
                    NULL, ESPAPERPLAY_UI_KEY_INPUT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "key input task create failed");
        return ESP_ERR_NO_MEM;
    }
#if ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST
    if (xTaskCreate(ui_key_selftest_task, "ui_key_selftest",
                    ESPAPERPLAY_UI_SELFTEST_TASK_STACK_SIZE, NULL,
                    ESPAPERPLAY_UI_SELFTEST_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "key selftest task create failed");
        return ESP_ERR_NO_MEM;
    }
#endif
    ESP_LOGI(TAG, "input dispatcher started (key -> page on_key, touch -> indev + page on_touch)");
    return ESP_OK;
}
