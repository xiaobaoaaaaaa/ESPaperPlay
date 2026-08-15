/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_display.h
 * @brief 显示参数服务：屏幕分辨率运行时配置。
 *
 * 驱动（EPD）/ 渲染后端（GUI）/ LVGL 移植层统一从这里读取宽高，取代
 * 编译期硬编码的 ESPAPERPLAY_DISPLAY_WIDTH/HEIGHT，从而支持不同分辨率
 * 与比例的面板（如横屏 800x480、竖屏 480x800，或其他尺寸）。
 *
 * 默认值 = 板级配置宏（800x480）；如需切换，在初始化各子系统之前调用
 * espaperplay_display_set()（例如 app_main 最早期 / 板级配置加载处），
 * 之后 EPD/GUI/LVGL 按新分辨率初始化（重启生效）。
 *
 * 注意：EPD 面板物理像素固定，运行期实时旋转（内容旋转 90/180/270）
 * 不在本服务范围；"横竖屏"通过面板安装方向 + 此处配置实现。
 */

/**
 * @brief 设置有效显示区分辨率（须在 EPD/GUI/LVGL 初始化之前调用）。
 *
 * @param width  显示区宽度（像素，建议 8 的倍数——1bpp 按字节寻址）。
 * @param height 显示区高度（像素）。
 *
 * @return ESP_OK；参数非法（0）返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t espaperplay_display_set(uint16_t width, uint16_t height);

/** 当前显示区宽度（像素）。 */
uint16_t espaperplay_display_width(void);

/** 当前显示区高度（像素）。 */
uint16_t espaperplay_display_height(void);

#ifdef __cplusplus
}
#endif
