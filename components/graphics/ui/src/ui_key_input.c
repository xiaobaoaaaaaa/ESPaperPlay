/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "espaperplay_gui_lv.h"
#include "espaperplay_input.h"
#include "espaperplay_ui.h"

static const char *TAG = "ESPaperPlay_UI";

#define ESPAPERPLAY_UI_KEY_INPUT_TASK_STACK_SIZE 4096
#define ESPAPERPLAY_UI_KEY_INPUT_TASK_PRIORITY 4

/* ====================================================================
 * 按键分发：input 事件队列 -> LVGL 线程 -> 栈顶页面 on_key
 * ====================================================================
 *
 * 按键分发任务阻塞在 espaperplay_input_get_event() 上（BOOT 键等真实
 * 事件与自检注入的合成事件走同一条队列，读取路径完全一致）；收到事件后
 * 经 espaperplay_gui_lv_call 投递到 LVGL 线程执行（LVGL 非线程安全，
 * 页面导航 / 控件更新必须串行在 LVGL 线程内），最终由页面栈转发给
 * 栈顶页面的 on_key 钩子——导航决策属于页面自身。
 */

/** LVGL 线程内执行：把按键事件转发给栈顶页面（arg 指向任务栈上的事件副本）。 */
static void ui_key_dispatch_cb(void *arg) {
    const espaperplay_input_event_t *event = (const espaperplay_input_event_t *)arg;

    ESP_LOGI(TAG, "key event: id=%u action=%s press=%u ms", event->key_id,
             espaperplay_input_key_action_str(event->key_action), event->key_press_time_ms);

    if (event->type == ESPAPERPLAY_INPUT_EVENT_KEY) {
        espaperplay_ui_page_handle_key_lv(event);
    }
    /* TOUCH 事件：触摸驱动接入后在此转发（LVGL 指针输入），当前阶段忽略。 */
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

/** 按键分发任务：阻塞读取输入队列，事件投递到 LVGL 线程处理。 */
static void ui_key_input_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "key input task started");

    for (;;) {
        espaperplay_input_event_t event;
        const esp_err_t err = espaperplay_input_get_event(&event, portMAX_DELAY);
        if (err != ESP_OK) {
            /* 输入子系统未初始化等异常：避免忙等空转。 */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        /* 高频 LONG_PRESS_HOLD 已在 input 层节流（500ms 一个），此处统一
         * 同步投递：gui_lv_call 的完成信号是全局单标志（单调用方语义），
         * 不可跳过等待，否则 HOLD 的完成信号会污染后续事件（如
         * LONG_PRESS_UP）的完成同步。 */
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
    ESP_LOGI(TAG, "key input dispatcher started");
    return ESP_OK;
}
