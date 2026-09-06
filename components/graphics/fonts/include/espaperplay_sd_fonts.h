/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file espaperplay_sd_fonts.h
 * @brief SD 卡完整字体目录 → LVGL 文件系统盘符（内部头文件）。
 *
 * 在 Flash 字体分区盘符 'A:'（espaperplay_fonts.h）之外，额外注册盘符
 * ESPAPERPLAY_SD_FONT_DRIVE_LETTER（'B:'），把对 SD 卡 ESPAPERPLAY_FONTS_SD_DIR
 * 目录（默认 /sdcard/system/fonts）的访问代理到标准 POSIX 文件 API。
 * 当 SD 卡挂载且该目录存在完整字库时，字体组件优先从此加载（而非 Flash
 * 裁剪子集）。
 *
 * 本头文件为字体组件内部使用；上层（GUI 屏幕）只通过 espaperplay_fonts_load()
 * 间接使用，无需感知盘符差异。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册 SD 字体盘符（'B:'）。
 *
 * 需在 LVGL 初始化（lv_init）之后调用；重复调用为幂等操作。注册本身
 * 不依赖 SD 卡是否已挂载（挂载与否由底层文件系统在执行时决定）。
 *
 * @return ESP_OK 成功。
 */
esp_err_t espaperplay_sd_fonts_init(void);

/**
 * @brief 组装 SD 卡字体文件的 LVGL 路径，如 "B:NotoSansSC_Regular.ttf"。
 *
 * @param[in]  file_name SD 卡 fonts 目录内的文件名（无路径分隔符）。
 * @param[out] buf       输出缓冲区。
 * @param[in]  buf_size  缓冲区大小。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE 参数错误。
 */
esp_err_t espaperplay_sd_fonts_get_path(const char *file_name, char *buf, size_t buf_size);

/**
 * @brief 检查 SD 卡 fonts 目录是否存在指定字体文件。
 *
 * 仅当 SD 卡已挂载（espaperplay_storage_is_mounted()）且文件真实存在时
 * 返回 true。
 *
 * @param file_name 文件名（无路径分隔符）。
 * @return 存在返回 true，否则 false。
 */
bool espaperplay_sd_fonts_exists(const char *file_name);

#ifdef __cplusplus
}
#endif
