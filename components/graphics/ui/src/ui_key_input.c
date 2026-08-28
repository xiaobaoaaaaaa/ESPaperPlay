/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

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
 *     UI_TOUCH_BATCH_WINDOW_MS 窗口批量**异步**投递（gui_lv_post，不等待
 *     执行完成），批内事件逐条转发给页面 on_touch——既保留全部中间坐标，
 *     又不阻塞本任务：LVGL 线程被渲染占用时（e-paper 页面重绘可达数百
 *     ms~数 s）同步等待会把本任务卡住、触摸队列打满丢帧（indev 释放沿
 *     丢失导致点击失效）。展示帧可丢，指针状态不受影响。
 */

/** LVGL 线程内执行：把按键事件转发给栈顶页面（arg 指向任务栈上的事件副本）。 */
static void ui_key_dispatch_cb(void *arg) {
    const espaperplay_input_event_t *event = (const espaperplay_input_event_t *)arg;
    const uint32_t cb_start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    ESP_LOGI(TAG, "key event: id=%u action=%s press=%u ms", event->key_id,
             espaperplay_input_key_action_str(event->key_action), event->key_press_time_ms);

    espaperplay_ui_page_handle_key_lv(event);
    const uint32_t cb_cost_ms = (uint32_t)(esp_timer_get_time() / 1000) - cb_start_ms;
    ESP_LOGD(TAG, "key dispatch cb done cost %u ms action=%s", (unsigned)cb_cost_ms,
             espaperplay_input_key_action_str(event->key_action));
    if (cb_cost_ms > 100) {
        ESP_LOGW(TAG, "key dispatch cb cost %u ms — 页面 on_key 阻塞过长，可能导致后续触摸超时",
                 (unsigned)cb_cost_ms);
    }
}

/** 触摸批量投递描述（静态池缓冲 + 数量）。 */
typedef struct {
    espaperplay_input_event_t events[UI_TOUCH_BATCH_MAX_EVENTS]; /*!< 本批事件数组 */
    uint16_t count;                                              /*!< 本批事件数 */
    bool in_use;                                                 /*!< 是否已被投递待执行 */
} ui_touch_batch_buf_t;

/** 批缓冲池深度：LVGL 线程繁忙时投递积压的上限（超出即丢弃展示帧）。 */
#define UI_TOUCH_BATCH_POOL_DEPTH 4

/** 批缓冲静态池（分发任务申请 / LVGL 线程释放，临界区保护）。 */
static ui_touch_batch_buf_t s_batch_pool[UI_TOUCH_BATCH_POOL_DEPTH];
static portMUX_TYPE s_batch_lock = portMUX_INITIALIZER_UNLOCKED;

/** 在途批数（已投递未执行）。LVGL 忙时在途达到上限即跳过投递（展示帧
 * 可丢），避免池耗尽后只能丢最新帧、且告警刷屏。 */
#define UI_TOUCH_BATCH_IN_FLIGHT_MAX 2
static volatile uint32_t s_batch_in_flight = 0;
static uint32_t s_batch_skip_log_ms = 0; /*!< 跳过投递告警节流（毫秒） */

/** 从池中申请一个空闲批缓冲（无空闲返回 NULL）。 */
static ui_touch_batch_buf_t *ui_touch_batch_buf_claim(void) {
    ui_touch_batch_buf_t *buf = NULL;
    portENTER_CRITICAL(&s_batch_lock);
    for (int i = 0; i < UI_TOUCH_BATCH_POOL_DEPTH; i++) {
        if (!s_batch_pool[i].in_use) {
            s_batch_pool[i].in_use = true;
            buf = &s_batch_pool[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_batch_lock);
    return buf;
}

/** 归还批缓冲（LVGL 线程回调执行完后调用）。 */
static void ui_touch_batch_buf_release(ui_touch_batch_buf_t *buf) {
    portENTER_CRITICAL(&s_batch_lock);
    buf->in_use = false;
    portEXIT_CRITICAL(&s_batch_lock);
}

/** LVGL 线程内执行：把批量触摸事件逐条转发给栈顶页面（on_touch）。 */
static void ui_touch_batch_dispatch_cb(void *arg) {
    ui_touch_batch_buf_t *batch = (ui_touch_batch_buf_t *)arg;
    const uint32_t cb_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGD(TAG, "touch batch cb: count=%u first seq=%u pressed=%u (%u,%u) @%u ms", batch->count,
             batch->count > 0 ? batch->events[0].touch_seq : 0,
             batch->count > 0 ? batch->events[0].touch_pressed : 0,
             batch->count > 0 ? batch->events[0].point.x : 0,
             batch->count > 0 ? batch->events[0].point.y : 0, (unsigned)cb_start_ms);
    for (uint16_t i = 0; i < batch->count; i++) {
        const espaperplay_input_event_t *ev = &batch->events[i];
        ESP_LOGD(TAG, "touch batch cb: [%u/%u] seq=%u pressed=%u (%u,%u) points=%u", i + 1,
                 batch->count, ev->touch_seq, ev->touch_pressed, ev->point.x, ev->point.y,
                 ev->touch_points);
        espaperplay_ui_page_handle_touch_lv(ev);
    }
    const uint32_t cb_cost_ms = (uint32_t)(esp_timer_get_time() / 1000) - cb_start_ms;
    ESP_LOGD(TAG, "touch batch cb done: count=%u cost %u ms", batch->count, (unsigned)cb_cost_ms);
    if (cb_cost_ms > 100) {
        ESP_LOGW(TAG, "touch batch cb cost %u ms count=%u — 页面 on_touch 阻塞过长",
                 (unsigned)cb_cost_ms, batch->count);
    }
    s_batch_in_flight--;
    ui_touch_batch_buf_release(batch);
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

    /* 触摸批量投递累积缓冲（本任务独占；投递时拷贝进静态池缓冲，
     * 由 LVGL 线程回调执行完后归还——异步投递后本缓冲立即复用）。 */
    espaperplay_input_event_t touch_batch[UI_TOUCH_BATCH_MAX_EVENTS];
    uint16_t touch_batch_count = 0;
    uint32_t touch_batch_start_ms = 0;
    uint32_t touch_total_events = 0;
    uint32_t touch_total_batches = 0;
    uint32_t touch_total_dropped_batches = 0;

    for (;;) {
        espaperplay_input_event_t event;
        const uint32_t wait_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const esp_err_t err = espaperplay_input_get_event(&event, portMAX_DELAY);
        const uint32_t wait_cost_ms = (uint32_t)(esp_timer_get_time() / 1000) - wait_start_ms;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "input get_event failed: %s wait_cost=%u ms", esp_err_to_name(err),
                     (unsigned)wait_cost_ms);
            /* 输入子系统未初始化等异常：避免忙等空转。 */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (wait_cost_ms > 500) {
            ESP_LOGD(TAG, "input get_event wait %u ms type=%d", (unsigned)wait_cost_ms,
                     (int)event.type);
        }

        if (event.type == ESPAPERPLAY_INPUT_EVENT_TOUCH) {
            touch_total_events++;
            ESP_LOGD(TAG,
                     "dispatcher: touch event seq=%u pressed=%u (%u,%u) points=%u "
                     "batch_count=%u wait_cost=%u ms total_events=%u",
                     event.touch_seq, event.touch_pressed, event.point.x, event.point.y,
                     event.touch_points, touch_batch_count, (unsigned)wait_cost_ms,
                     (unsigned)touch_total_events);
            /* 指针 indev 状态：逐事件直接更新（临界区保护），无 LVGL
             * 往返延迟。 */
            espaperplay_ui_touch_update(&event);

            /* 页面展示：窗口内累积批量投递（保留全部中间点）；释放事件
             * 必须立即转发（笔画收尾）。投递为异步（gui_lv_post）：
             * 不等待 LVGL 线程执行完成，避免渲染繁忙时阻塞本任务。 */
            const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (touch_batch_count == 0) {
                touch_batch_start_ms = now_ms;
            }
            if (touch_batch_count >= UI_TOUCH_BATCH_MAX_EVENTS) {
                ESP_LOGW(TAG,
                         "dispatcher: touch batch overflow seq=%u — 批已满 %u 未及时投递，"
                         "可能丢失中间点",
                         event.touch_seq, touch_batch_count);
            } else {
                touch_batch[touch_batch_count++] = event;
            }

            const bool batch_full = touch_batch_count >= UI_TOUCH_BATCH_MAX_EVENTS;
            const bool window_elapsed = now_ms - touch_batch_start_ms >= UI_TOUCH_BATCH_WINDOW_MS;
            const bool is_release = event.touch_pressed == 0;
            if (batch_full || window_elapsed || is_release) {
                const uint32_t batch_age_ms = now_ms - touch_batch_start_ms;
                ESP_LOGD(TAG,
                         "dispatcher: dispatch touch batch count=%u age=%u ms reason=%s "
                         "seq %u..%u",
                         touch_batch_count, (unsigned)batch_age_ms,
                         batch_full ? "full" : (is_release ? "release" : "window"),
                         touch_batch_count > 0 ? touch_batch[0].touch_seq : 0,
                         touch_batch_count > 0 ? touch_batch[touch_batch_count - 1].touch_seq : 0);
                /* 异步投递：不等待 LVGL 线程执行完成。LVGL 被渲染占用时
                 * （e-paper 页面重绘可达数百 ms~数 s）同步等待会把分发任务
                 * 卡住、触摸队列打满丢帧（indev 释放沿丢失导致点击失效）。
                 * 展示帧可丢，指针状态不受影响（已逐事件直接更新）。
                 * 在途批达到上限即跳过投递（合并丢弃中间展示帧）。 */
                if (s_batch_in_flight >= UI_TOUCH_BATCH_IN_FLIGHT_MAX) {
                    touch_total_dropped_batches++;
                    const uint32_t now_ms2 = (uint32_t)(esp_timer_get_time() / 1000);
                    if (now_ms2 - s_batch_skip_log_ms >= 1000) {
                        s_batch_skip_log_ms = now_ms2;
                        ESP_LOGW(TAG,
                                 "touch batch skipped (in_flight=%u >= %u) count=%u "
                                 "total_dropped=%u — LVGL 线程积压，展示帧合并丢弃",
                                 (unsigned)s_batch_in_flight,
                                 (unsigned)UI_TOUCH_BATCH_IN_FLIGHT_MAX, touch_batch_count,
                                 (unsigned)touch_total_dropped_batches);
                    } else {
                        ESP_LOGD(TAG, "touch batch skipped in_flight=%u count=%u",
                                 (unsigned)s_batch_in_flight, touch_batch_count);
                    }
                    touch_batch_count = 0;
                    continue;
                }
                ui_touch_batch_buf_t *buf = ui_touch_batch_buf_claim();
                if (buf == NULL) {
                    touch_total_dropped_batches++;
                    ESP_LOGW(TAG,
                             "touch batch pool exhausted, drop batch count=%u age=%u ms "
                             "total_dropped=%u — LVGL 线程积压",
                             touch_batch_count, (unsigned)batch_age_ms,
                             (unsigned)touch_total_dropped_batches);
                } else {
                    memcpy(buf->events, touch_batch,
                           (size_t)touch_batch_count * sizeof(espaperplay_input_event_t));
                    buf->count = touch_batch_count;
                    const esp_err_t post_err =
                        espaperplay_gui_lv_post(ui_touch_batch_dispatch_cb, buf);
                    touch_total_batches++;
                    if (post_err != ESP_OK) {
                        touch_total_dropped_batches++;
                        ui_touch_batch_buf_release(buf);
                        ESP_LOGW(TAG,
                                 "post touch batch to LVGL failed: %s count=%u age=%u ms "
                                 "total_batches=%u dropped=%u — 本批 %u 个展示点丢失",
                                 esp_err_to_name(post_err), touch_batch_count,
                                 (unsigned)batch_age_ms, (unsigned)touch_total_batches,
                                 (unsigned)touch_total_dropped_batches, touch_batch_count);
                    } else {
                        s_batch_in_flight++;
                        ESP_LOGD(TAG, "post touch batch ok count=%u age=%u ms in_flight=%u",
                                 touch_batch_count, (unsigned)batch_age_ms,
                                 (unsigned)s_batch_in_flight);
                    }
                }
                touch_batch_count = 0;
            } else {
                ESP_LOGD(TAG, "dispatcher: touch batch buffered count=%u age=%u ms seq=%u",
                         touch_batch_count, (unsigned)(now_ms - touch_batch_start_ms),
                         event.touch_seq);
            }
            continue;
        }

        /* 按键事件：同步投递到 LVGL 线程处理。 */
        ESP_LOGD(TAG, "dispatcher: key event id=%u action=%s press=%u ms wait_cost=%u ms",
                 event.key_id, espaperplay_input_key_action_str(event.key_action),
                 event.key_press_time_ms, (unsigned)wait_cost_ms);
        const uint32_t call_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const esp_err_t call_err = espaperplay_gui_lv_call(ui_key_dispatch_cb, &event, 1000);
        const uint32_t call_cost_ms = (uint32_t)(esp_timer_get_time() / 1000) - call_start_ms;
        if (call_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "dispatch key event to LVGL failed: %s id=%u action=%s cost=%u ms — 按键丢失",
                     esp_err_to_name(call_err), event.key_id,
                     espaperplay_input_key_action_str(event.key_action), (unsigned)call_cost_ms);
        } else {
            ESP_LOGD(TAG, "dispatch key done id=%u action=%s cost=%u ms", event.key_id,
                     espaperplay_input_key_action_str(event.key_action), (unsigned)call_cost_ms);
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
