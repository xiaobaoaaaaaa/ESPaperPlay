/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "freertos/FreeRTOS.h"

#include "esp_log.h"

#include "espaperplay_input.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * LVGL 触摸指针（indev）接入
 * ====================================================================
 *
 * 数据流：GT911 驱动 -> input 触摸队列 -> LVGL read_cb 直接排空（本模块，
 * LVGL 线程内）。无独立分发任务、无跨线程共享状态：
 *   - read_cb 每个读周期（~30ms）把触摸队列全量排空；
 *   - 队列中的每个事件逐条转发给栈顶页面 on_touch（轨迹绘制需要全部
 *     中间坐标点；本回调已在 LVGL 线程内，无需跨线程投递）；
 *   - 按压状态机在排空时推进；若完整的按下+释放都落在两次 read 之间
 *     （LVGL 线程被渲染阻塞时常见），点击入锁存 FIFO，后续 read 逐个
 *     补报 PRESSED+RELEASED，保证 LVGL 注册到完整点击。
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

/** 触摸状态（仅 LVGL 线程 read_cb 访问，无需加锁）。 */
typedef struct {
    bool pressed;          /*!< 当前是否按下 */
    uint16_t x;            /*!< 本帧第一个触摸点（面板物理坐标） */
    uint16_t y;            /*!< 本帧第一个触摸点（面板物理坐标） */
    uint16_t seq;          /*!< 已接受的触摸帧序号（同帧其余点忽略） */
    uint32_t ts_ms;        /*!< 最近一次状态更新时刻（lv_tick 毫秒） */
    bool read_since_press; /*!< 按下后是否已上报过按下态（用于点击锁存判定） */
    /* 点击锁存 FIFO：完整的按下+释放发生在两次 read 之间时入队（快速连击
     * 可能有多个），read_cb 每次补报一个（PRESSED -> RELEASED 交替）。 */
    uint16_t pend_x[UI_TOUCH_PENDING_CLICK_MAX]; /*!< 锁存点击坐标（面板物理坐标） */
    uint16_t pend_y[UI_TOUCH_PENDING_CLICK_MAX]; /*!< 锁存点击坐标（面板物理坐标） */
    uint8_t pend_head;                           /*!< 队头下标（最旧待补报点击） */
    uint8_t pend_count;                          /*!< 队列中的待补报点击数 */
} ui_touch_state_t;

static ui_touch_state_t s_state = {0};
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
 * @brief LVGL 指针 read_cb：排空触摸队列并返回当前按压状态与物理坐标。
 *
 * LVGL 以 read_timer 周期（LV_DEF_REFR_PERIOD）调用；按下期间持续返回
 * PRESSED 才能产生点击/按压语义。坐标必须为面板物理坐标：LVGL 内核
 * （indev_pointer_proc）会按显示旋转自动换算为逻辑坐标。
 *
 * 排空阶段：队列中的每个事件先逐条转发给栈顶页面 on_touch（轨迹绘制），
 * 再推进按压状态机；完整的按下+释放落在两次 read 之间时点击入锁存 FIFO。
 * 上报阶段：先补报锁存点击（PRESSED/RELEASED 交替），再上报当前状态；
 * 若释放帧被遗漏（中断沿丢失等），超过 UI_TOUCH_AUTO_RELEASE_MS 无新帧
 * 则兜底自动释放。
 */
static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;

    /* ---- 排空阶段：消费 input 触摸队列（LVGL 线程内，无需加锁）---- */
    espaperplay_input_event_t ev;
    while (espaperplay_input_try_get_touch(&ev) == ESP_OK) {
        /* 页面展示：逐条转发栈顶页面 on_touch（轨迹绘制需要全部中间点）。 */
        espaperplay_ui_page_handle_touch_lv(&ev);

        if (!ev.touch_pressed) {
            /* 释放帧：立即置释放（点击语义依赖精确的释放沿）。若本次按下
             * 尚未上报过（完整按下+释放落在两次 read 之间，LVGL 线程被
             * 渲染阻塞时常见），点击入锁存 FIFO——后续 read 逐个补报
             * PRESSED+RELEASED，否则该点击会被吞。快速连击依次排队
             * （队列满丢最旧——最早的点击已过时）。 */
            if (s_state.pressed && !s_state.read_since_press) {
                if (s_state.pend_count < UI_TOUCH_PENDING_CLICK_MAX) {
                    const uint8_t tail = (uint8_t)((s_state.pend_head + s_state.pend_count) %
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
            s_state.pressed = false;
            s_state.ts_ms = lv_tick_get();
        } else if (ev.touch_seq != s_state.seq) {
            /* 按下帧：只接受本帧第一个点（多指时指针不跳变）。 */
            s_state.pressed = true;
            s_state.seq = ev.touch_seq;
            s_state.x = ev.point.x;
            s_state.y = ev.point.y;
            s_state.ts_ms = lv_tick_get();
            s_state.read_since_press = false;
        }
    }

    /* ---- 上报阶段：锁存点击优先，其次当前状态 ---- */
    /* 点击锁存补报：完整的按下+释放发生在两次 read 之间（LVGL 线程被
     * 渲染/刷新阻塞数百 ms 时常见），直接读状态只会看到 RELEASED，点击
     * 被吞。每次 read 从 FIFO 补报一个（PRESSED），下次 read 强制报
     * RELEASED——LVGL 依此逐个注册完整点击。 */
    if (s_force_release_next) {
        s_force_release_next = false;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (s_state.pend_count > 0) {
        const uint16_t lx = s_state.pend_x[s_state.pend_head];
        const uint16_t ly = s_state.pend_y[s_state.pend_head];
        s_state.pend_head = (uint8_t)((s_state.pend_head + 1) % UI_TOUCH_PENDING_CLICK_MAX);
        s_state.pend_count--;
        s_force_release_next = true;
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = (int32_t)lx;
        data->point.y = (int32_t)ly;
        return;
    }

    if (s_state.pressed && (lv_tick_get() - s_state.ts_ms) > UI_TOUCH_AUTO_RELEASE_MS) {
        s_state.pressed = false;
        ESP_LOGD(TAG, "touch: auto released (no frame for %u ms)", UI_TOUCH_AUTO_RELEASE_MS);
    }

    if (s_state.pressed) {
        s_state.read_since_press = true;
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = (int32_t)s_state.x;
        data->point.y = (int32_t)s_state.y;
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
