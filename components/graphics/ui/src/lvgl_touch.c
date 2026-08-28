/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "esp_log.h"

#include "espaperplay_ui_touch.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * LVGL 触摸指针（indev）接入
 * ====================================================================
 *
 * 数据流：GT911 驱动 -> input 触摸队列 -> 输入分发任务 -> 本模块共享
 * 状态（任意线程写入，临界区保护）-> LVGL read_cb（LVGL 线程周期读取）。
 *
 * 坐标约定（与 LVGL 官方旋转方案一致）：
 *   - 触摸驱动上报面板物理坐标（800x480），read_cb 原样上报；
 *   - LVGL 内核 indev_pointer_proc 会按显示旋转自动调用
 *     lv_display_rotate_point() 换算为逻辑坐标，因此 read_cb 严禁再
 *     手动换算（否则双重旋转，控件命中错位）；
 *   - 页面直接绘制轨迹等场景通过 espaperplay_ui_touch_map_to_lv()
 *     复用同一个 LVGL 旋转约定。
 */

/** 丢帧兜底：超过该时间没有新触摸帧（含释放帧）则自动置释放。 */
#define UI_TOUCH_AUTO_RELEASE_MS 200

/** 点击锁存 FIFO 深度：LVGL 忙碌期间到达的快速连击排队上限（超出丢最旧）。 */
#define UI_TOUCH_PENDING_CLICK_MAX 8

/** 共享触摸状态（dispatcher 任务写入 / LVGL read_cb 读取）。 */
typedef struct {
    bool pressed;          /*!< 当前是否按下 */
    uint16_t x;            /*!< 本帧第一个触摸点（面板物理坐标） */
    uint16_t y;            /*!< 本帧第一个触摸点（面板物理坐标） */
    uint16_t seq;          /*!< 已接受的触摸帧序号（同帧其余点忽略） */
    uint32_t ts_ms;        /*!< 最近一次状态更新时刻（lv_tick 毫秒） */
    bool read_since_press; /*!< 按下后 LVGL 是否已读过（用于点击锁存判定） */
    /* 点击锁存 FIFO：完整的按下+释放发生在两次 read 之间时入队（快速连击
     * 可能有多个），read_cb 每次补报一个（PRESSED -> RELEASED 交替）。 */
    uint16_t pend_x[UI_TOUCH_PENDING_CLICK_MAX]; /*!< 锁存点击坐标（面板物理坐标） */
    uint16_t pend_y[UI_TOUCH_PENDING_CLICK_MAX]; /*!< 锁存点击坐标（面板物理坐标） */
    uint8_t pend_head;                           /*!< 队头下标（最旧待补报点击） */
    uint8_t pend_count;                          /*!< 队列中的待补报点击数 */
} ui_touch_state_t;

static ui_touch_state_t s_state = {0};
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static lv_indev_t *s_indev = NULL;

/* 点击锁存补报状态（仅 LVGL 线程 read_cb 访问，无需加锁）。 */
static bool s_force_release_next = false; /*!< 锁存点击已补报按下，下次强制报释放 */

/** 面板物理坐标 -> LVGL 逻辑坐标：复用 LVGL 内核对 indev 坐标的旋转约定。 */
void espaperplay_ui_touch_map_to_lv(uint16_t x, uint16_t y, lv_point_t *out) {
    out->x = (int32_t)x;
    out->y = (int32_t)y;
    lv_display_rotate_point(lv_display_get_default(), out);
}

/**
 * @brief LVGL 指针 read_cb：返回当前按压状态与面板物理坐标。
 *
 * LVGL 以 read_timer 周期（LV_DEF_REFR_PERIOD）调用；按下期间持续返回
 * PRESSED 才能产生点击/按压语义。坐标必须为面板物理坐标：LVGL 内核
 * （indev_pointer_proc）会按显示旋转自动换算为逻辑坐标。释放帧到达时
 * update() 立即置释放；若释放帧被遗漏（中断沿丢失等），超过
 * UI_TOUCH_AUTO_RELEASE_MS 无新帧则兜底自动释放。
 */
static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;

    /* 点击锁存补报：完整的按下+释放发生在两次 read 之间（LVGL 线程被
     * 渲染/刷新阻塞数百 ms 时常见），直接读共享状态只会看到 RELEASED，
     * 点击被吞。每次 read 从 FIFO 补报一个（PRESSED），下次 read 强制报
     * RELEASED——LVGL 依此逐个注册完整点击。 */
    if (s_force_release_next) {
        s_force_release_next = false;
        data->state = LV_INDEV_STATE_RELEASED;
        ESP_LOGD(TAG, "touch indev: latch report RELEASED");
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (s_state.pend_count > 0) {
        const uint16_t lx = s_state.pend_x[s_state.pend_head];
        const uint16_t ly = s_state.pend_y[s_state.pend_head];
        s_state.pend_head = (uint8_t)((s_state.pend_head + 1) % UI_TOUCH_PENDING_CLICK_MAX);
        s_state.pend_count--;
        const uint8_t remain = s_state.pend_count;
        portEXIT_CRITICAL(&s_state_lock);
        s_force_release_next = true;
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = (int32_t)lx;
        data->point.y = (int32_t)ly;
        ESP_LOGW(TAG, "touch indev: latch report PRESSED (%u,%u) remain=%u — 点击在 LVGL "
                      "忙碌期间到达",
                 lx, ly, remain);
        return;
    }
    portEXIT_CRITICAL(&s_state_lock);

    portENTER_CRITICAL(&s_state_lock);
    bool pressed = s_state.pressed;
    const uint16_t x = s_state.x;
    const uint16_t y = s_state.y;
    const uint32_t ts_ms = s_state.ts_ms;
    const uint16_t seq = s_state.seq;
    portEXIT_CRITICAL(&s_state_lock);

    if (pressed && (lv_tick_get() - ts_ms) > UI_TOUCH_AUTO_RELEASE_MS) {
        portENTER_CRITICAL(&s_state_lock);
        s_state.pressed = false;
        portEXIT_CRITICAL(&s_state_lock);
        pressed = false;
        ESP_LOGW(TAG, "touch indev: auto released seq=%u no frame for %u ms (x=%u y=%u)",
                 seq, UI_TOUCH_AUTO_RELEASE_MS, x, y);
    } else if (pressed) {
        portENTER_CRITICAL(&s_state_lock);
        s_state.read_since_press = true;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGD(TAG, "touch indev: read PRESSED seq=%u (%u,%u) age=%u ms", seq, x, y,
                 (unsigned)(lv_tick_get() - ts_ms));
    } else {
        ESP_LOGD(TAG, "touch indev: read RELEASED seq=%u age=%u ms", seq,
                 (unsigned)(lv_tick_get() - ts_ms));
    }

    if (pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = (int32_t)x;
        data->point.y = (int32_t)y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t espaperplay_ui_touch_init(void) {
    s_indev = lv_indev_create();
    if (s_indev == NULL) {
        ESP_LOGE(TAG, "touch indev create failed");
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, ui_touch_read_cb);
    /* 滚动触发阈值调低（默认 10px）：天气图表横向滚动更灵敏 */
    lv_indev_set_scroll_limit(s_indev, 5);
    ESP_LOGI(TAG, "touch pointer indev registered (auto release %u ms)", UI_TOUCH_AUTO_RELEASE_MS);
    return ESP_OK;
}

void espaperplay_ui_touch_update(const espaperplay_input_event_t *event) {
    const uint32_t now_tick = lv_tick_get();
    portENTER_CRITICAL(&s_state_lock);
    if (!event->touch_pressed) {
        /* 释放帧：立即置释放（点击语义依赖精确的释放沿）。若 LVGL 尚未
         * 读过本次按下（完整按下+释放落在两次 read 之间，LVGL 线程被
         * 渲染阻塞时常见），点击入锁存 FIFO——read_cb 逐个补报
         * PRESSED+RELEASED，否则该点击会被吞。快速连击依次排队（队列
         * 满丢最旧——最早的点击已过时）。 */
        if (s_state.pressed && !s_state.read_since_press) {
            if (s_state.pend_count < UI_TOUCH_PENDING_CLICK_MAX) {
                const uint8_t tail =
                    (uint8_t)((s_state.pend_head + s_state.pend_count) %
                              UI_TOUCH_PENDING_CLICK_MAX);
                s_state.pend_x[tail] = s_state.x;
                s_state.pend_y[tail] = s_state.y;
                s_state.pend_count++;
            } else {
                /* 队列满：丢最旧（head 前移），保留最新点击。 */
                s_state.pend_x[s_state.pend_head] = s_state.x;
                s_state.pend_y[s_state.pend_head] = s_state.y;
                s_state.pend_head =
                    (uint8_t)((s_state.pend_head + 1) % UI_TOUCH_PENDING_CLICK_MAX);
            }
        }
        const bool was_pressed = s_state.pressed;
        const uint8_t latched = s_state.pend_count;
        s_state.pressed = false;
        s_state.ts_ms = now_tick;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGD(TAG, "touch indev: update RELEASE seq=%u was_pressed=%d latched=%u @tick %u",
                 event->touch_seq, (int)was_pressed, latched, (unsigned)now_tick);
    } else if (event->touch_seq != s_state.seq) {
        /* 按下帧：只接受本帧第一个点（多指时指针不跳变）。 */
        s_state.pressed = true;
        s_state.seq = event->touch_seq;
        s_state.x = event->point.x;
        s_state.y = event->point.y;
        s_state.ts_ms = now_tick;
        s_state.read_since_press = false;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGD(TAG, "touch indev: update PRESSED seq=%u (%u,%u) points=%u @tick %u",
                 event->touch_seq, event->point.x, event->point.y, event->touch_points,
                 (unsigned)now_tick);
    } else {
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGD(TAG, "touch indev: update skip duplicate seq=%u (%u,%u)", event->touch_seq,
                 event->point.x, event->point.y);
    }
}
