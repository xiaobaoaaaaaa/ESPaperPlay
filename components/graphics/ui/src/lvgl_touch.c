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
 * 坐标：触摸驱动上报面板物理坐标（800x480）；LVGL 控件位于逻辑坐标系
 * （test 页双击旋转后逻辑分辨率变为 480x800）。read_cb 与页面均通过
 * espaperplay_ui_touch_map_to_lv() 换算，该映射与 lvgl_port.c flush
 * 回调的旋转映射互为逆变换，旋转后触摸仍与显示内容对齐。
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

/** 面板物理坐标 -> LVGL 逻辑坐标（LVGL 线程内，使用默认显示）。 */
static void ui_touch_map_inner(uint16_t x, uint16_t y, lv_point_t *out) {
    lv_display_t *disp = lv_display_get_default();
    const lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    const int32_t lw = lv_display_get_horizontal_resolution(disp); /* 逻辑宽 */
    const int32_t lh = lv_display_get_vertical_resolution(disp);   /* 逻辑高 */

    switch (rotation) {
    case LV_DISPLAY_ROTATION_90: /* 物理 (x, y) -> 逻辑 (y, lh-1-x) */
        out->x = (int32_t)y;
        out->y = lh - 1 - (int32_t)x;
        break;
    case LV_DISPLAY_ROTATION_180: /* 物理 (x, y) -> 逻辑 (lw-1-x, lh-1-y) */
        out->x = lw - 1 - (int32_t)x;
        out->y = lh - 1 - (int32_t)y;
        break;
    case LV_DISPLAY_ROTATION_270: /* 物理 (x, y) -> 逻辑 (lw-1-y, x) */
        out->x = lw - 1 - (int32_t)y;
        out->y = (int32_t)x;
        break;
    case LV_DISPLAY_ROTATION_0:
    default:
        out->x = (int32_t)x;
        out->y = (int32_t)y;
        break;
    }
}

/**
 * @brief LVGL 指针 read_cb：返回当前按压状态与逻辑坐标。
 *
 * LVGL 以 read_timer 周期（LV_DEF_REFR_PERIOD）调用；按下期间持续返回
 * PRESSED 才能产生点击/按压语义。释放帧到达时 update() 立即置释放；
 * 若释放帧被遗漏（中断沿丢失等），超过 UI_TOUCH_AUTO_RELEASE_MS 无新
 * 帧则兜底自动释放。
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
        ui_touch_map_inner(x, y, &data->point);
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

void espaperplay_ui_touch_map_to_lv(uint16_t x, uint16_t y, lv_point_t *out) {
    ui_touch_map_inner(x, y, out);
}
