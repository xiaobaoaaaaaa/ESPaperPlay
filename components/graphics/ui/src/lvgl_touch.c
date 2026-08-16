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

/** 共享触摸状态（dispatcher 任务写入 / LVGL read_cb 读取）。 */
typedef struct {
    bool pressed;   /*!< 当前是否按下 */
    uint16_t x;     /*!< 本帧第一个触摸点（面板物理坐标） */
    uint16_t y;     /*!< 本帧第一个触摸点（面板物理坐标） */
    uint16_t seq;   /*!< 已接受的触摸帧序号（同帧其余点忽略） */
    uint32_t ts_ms; /*!< 最近一次状态更新时刻（lv_tick 毫秒） */
} ui_touch_state_t;

static ui_touch_state_t s_state = {0};
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static lv_indev_t *s_indev = NULL;

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

    portENTER_CRITICAL(&s_state_lock);
    bool pressed = s_state.pressed;
    const uint16_t x = s_state.x;
    const uint16_t y = s_state.y;
    const uint32_t ts_ms = s_state.ts_ms;
    portEXIT_CRITICAL(&s_state_lock);

    if (pressed && (lv_tick_get() - ts_ms) > UI_TOUCH_AUTO_RELEASE_MS) {
        portENTER_CRITICAL(&s_state_lock);
        s_state.pressed = false;
        portEXIT_CRITICAL(&s_state_lock);
        pressed = false;
        ESP_LOGD(TAG, "touch: auto released (no frame for %u ms)", UI_TOUCH_AUTO_RELEASE_MS);
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
    ESP_LOGI(TAG, "touch pointer indev registered (auto release %u ms)", UI_TOUCH_AUTO_RELEASE_MS);
    return ESP_OK;
}

void espaperplay_ui_touch_update(const espaperplay_input_event_t *event) {
    portENTER_CRITICAL(&s_state_lock);
    if (!event->touch_pressed) {
        /* 释放帧：立即置释放（点击语义依赖精确的释放沿）。 */
        s_state.pressed = false;
        s_state.ts_ms = lv_tick_get();
    } else if (event->touch_seq != s_state.seq) {
        /* 按下帧：只接受本帧第一个点（多指时指针不跳变）。 */
        s_state.pressed = true;
        s_state.seq = event->touch_seq;
        s_state.x = event->point.x;
        s_state.y = event->point.y;
        s_state.ts_ms = lv_tick_get();
    }
    portEXIT_CRITICAL(&s_state_lock);
}
