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

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/** 按键泵周期（毫秒）：LVGL 线程内 lv_timer 排空按键队列的间隔。 */
#define UI_KEY_PUMP_PERIOD_MS 20

#if ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST
#define ESPAPERPLAY_UI_SELFTEST_TASK_STACK_SIZE 3072
#define ESPAPERPLAY_UI_SELFTEST_TASK_PRIORITY 3
#define ESPAPERPLAY_UI_SELFTEST_WAIT_MS 3000
#endif

/* ====================================================================
 * 输入消费：input 双队列 -> LVGL 线程直读 -> 页面钩子 / 触摸指针
 * ====================================================================
 *
 * 按键与触摸分别由 LVGL 线程内的两个入口直接消费（无独立分发任务、
 * 无跨线程投递）：
 *   - 按键：本文件创建的按键泵 lv_timer（UI_KEY_PUMP_PERIOD_MS 周期）
 *     排空按键队列，逐条转发给栈顶页面 on_key（导航/刷新内容）。按键
 *     稀疏（长按 HOLD 节流后 ~2/秒），20ms 泵周期延迟可忽略；
 *   - 触摸：触摸指针 indev 的 read_cb 排空触摸队列（见 lvgl_touch.c），
 *     逐条转发页面 on_touch 并驱动控件点击。
 *
 * 两个入口都运行在 LVGL 线程内，页面钩子可直接调用任意 lv_* API 与
 * espaperplay_ui_page_*_lv()，无需 espaperplay_gui_lv_call 往返。
 */

/** 按键泵（LVGL 线程内周期执行）：排空按键队列，逐条转发页面 on_key。 */
static void ui_key_pump_timer_cb(lv_timer_t *timer) {
    (void)timer;
    espaperplay_input_event_t event;
    while (espaperplay_input_try_get_key(&event) == ESP_OK) {
        ESP_LOGI(TAG, "key event: id=%u action=%s press=%u ms", event.key_id,
                 espaperplay_input_key_action_str(event.key_action), event.key_press_time_ms);
        espaperplay_ui_page_handle_key_lv(&event);
    }
}

/** 在 LVGL 线程内创建按键泵定时器（经 gui_lv_call 投递执行）。 */
static void ui_key_pump_create_cb(void *arg) {
    (void)arg;
    lv_timer_create(ui_key_pump_timer_cb, UI_KEY_PUMP_PERIOD_MS, NULL);
}

#if ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST

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
 * 独立任务运行，按键泵在此期间持续消费按键队列——注入事件与真实
 * 按键走完全相同的路径（input 按键队列 -> 按键泵 -> 页面钩子）。
 *
 * 用例 1：注入 SINGLE_CLICK，期望 home 的 on_key 压入测试页
 *         （页面栈深度 1 -> 2）；
 * 用例 2：注入 LONG_PRESS_UP，期望 测试页的 on_key 弹出返回 home
 *         （深度 2 -> 1，验证 pop 重建上一页）；
 * 用例 3：再次注入 SINGLE_CLICK，期望深度 1 -> 2（往返后系统仍存活、
 *         页面栈与按键泵链路工作正常）。
 */
static void ui_key_selftest_task(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "key selftest start (injecting synthetic key events)");
    vTaskDelay(pdMS_TO_TICKS(200)); /* 等待按键泵进入事件循环 */

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

esp_err_t espaperplay_ui_input_start(void) {
    /* 按键泵定时器必须在 LVGL 线程内创建（lv_timer 非线程安全）。 */
    const esp_err_t err = espaperplay_gui_lv_call(ui_key_pump_create_cb, NULL, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "key pump timer create failed: %s", esp_err_to_name(err));
        return err;
    }
#if ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST
    if (xTaskCreate(ui_key_selftest_task, "ui_key_selftest",
                    ESPAPERPLAY_UI_SELFTEST_TASK_STACK_SIZE, NULL,
                    ESPAPERPLAY_UI_SELFTEST_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "key selftest task create failed");
        return ESP_ERR_NO_MEM;
    }
#endif
    ESP_LOGI(TAG, "input consumer started (key pump %u ms + touch indev read, both in LVGL thread)",
             (unsigned)UI_KEY_PUMP_PERIOD_MS);
    return ESP_OK;
}
