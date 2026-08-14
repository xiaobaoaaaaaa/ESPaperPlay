/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_ui.h
 * @brief UI 页面层：构建 LVGL widget 树。
 *
 * 页面函数只创建/组织 LVGL 控件并注册事件回调，不接触像素、不调用渲染
 * 后端 API（LVGL 的无效区机制自动驱动 flush 回调链）。须在
 * espaperplay_gui_lv_start() 之后调用（页面依赖 LVGL 已初始化）。
 */

/**
 * @brief 展示最小就绪演示屏：白底 + 居中标签。
 */
void espaperplay_ui_demo_show(void);

/**
 * @brief 展示局部刷新压力测试屏。
 *
 * 多区域以不同节奏变化（200ms 小数字快刷 / 进度条 / 2s 大黑块位移触发
 * 大面积刷新），配合 worker 日志验证：异步刷新不阻塞 LVGL（状态行实测
 * 周期稳定）、脏区只刷小窗（worker 日志区域尺寸变化）。
 */
void espaperplay_ui_test_show(void);

#ifdef __cplusplus
}
#endif
