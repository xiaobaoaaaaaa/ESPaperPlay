/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"

#include "espaperplay_epd.h"
#include "espaperplay_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_gui.h
 * @brief GUI 抽象层。
 *
 * 定义未来基于 LVGL 的 UI 将要实现的接口。后续阶段本组件将拥有显示缓冲
 * （由 PSRAM 提供）、基于 EPD 的 LVGL 移植（通过 espaperplay_epd_*）以及
 * 触摸输入（通过 espaperplay_touch_*）。目前仅存在骨架生命周期 API，
 * 有意未链接 LVGL。
 */

/**
 * @brief 初始化 GUI 子系统。
 *
 * 分配显示帧缓冲并准备渲染后端。
 *
 * @note 当前仅为骨架：LVGL 集成为后续阶段。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_gui_init(void);

/**
 * @brief 启动 GUI 任务。
 *
 * 创建 GUI 渲染 / 事件循环 FreeRTOS 任务（未来为 LVGL 任务）。
 * 预期在 init 之后从 app_main 调用一次。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_gui_start(void);

#ifdef __cplusplus
}
#endif
