/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "espaperplay_input.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_ui_touch.h
 * @brief LVGL 触摸指针（indev）接入层。
 *
 * 把输入服务的触摸队列桥接到 LVGL 指针输入设备：
 *   - espaperplay_ui_touch_init() 创建 LV_INDEV_TYPE_POINTER 输入设备，
 *     注册 read_cb（LVGL 线程周期调用：排空 input 触摸队列，逐条转发
 *     页面 on_touch 并推进按压状态机）；
 *   - read_cb 上报**面板物理坐标**：LVGL 内核（indev_pointer_proc）会
 *     按显示旋转自动调用 lv_display_rotate_point() 换算为逻辑坐标，
 *     read_cb 严禁再手动换算（否则双重旋转导致控件命中错位）。
 *
 * 这样 LVGL 的按钮/滑块等控件即可响应触摸；页面级触摸展示
 * （坐标/点数等）仍走页面 on_touch 钩子（见 espaperplay_ui.h），页面
 * 直接绘制时用 espaperplay_ui_touch_map_to_lv() 复用同一旋转约定。
 */

/**
 * @brief 初始化触摸指针输入设备（在 LVGL display 创建之后调用）。
 *
 * 创建 pointer 类型 indev 并注册 read_cb；默认无光标（指针位置由
 * 页面自行绘制指示元素）。
 *
 * @return 成功返回 ESP_OK；indev 分配失败返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_ui_touch_init(void);

/**
 * @brief 把面板物理坐标换算为 LVGL 逻辑坐标（LVGL 线程内调用）。
 *
 * 直接复用 LVGL 内核的 indev 坐标旋转（lv_display_rotate_point），与
 * flush 回调使用的 lv_display_rotate_area 属同一旋转约定，供页面把
 * 触摸事件坐标绘制到逻辑坐标系（如测试页轨迹画板）。
 *
 * @param[in]  x   面板物理 X（0..物理宽-1）。
 * @param[in]  y   面板物理 Y（0..物理高-1）。
 * @param[out] out LVGL 逻辑坐标。
 */
void espaperplay_ui_touch_map_to_lv(uint16_t x, uint16_t y, lv_point_t *out);

#ifdef __cplusplus
}
#endif
