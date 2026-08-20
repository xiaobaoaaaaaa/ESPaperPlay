/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file espaperplay_fonts.h
 * @brief 只读字体资产：fonts 分区内存映射 + LVGL 文件系统盘符 + FreeType 字体加载。
 *
 * 构建期将 assets/fonts 下的 TTF 字体打包为 SPIFFS 镜像烧入 "fonts" 分区（见
 * CMakeLists.txt 的 spiffs_create_partition_assets），运行期通过
 * esp_mmap_assets 映射分区（字体数据不占 RAM），并注册为 LVGL 虚拟
 * 文件系统盘符，供 LVGL FreeType 以 "A:xxx.ttf" 路径加载。
 *
 * 字体渲染基于 LVGL FreeType 封装（CONFIG_LV_USE_FREETYPE）+ 外部
 * espressif/freetype 库（位图渲染模式，适配 e-paper 灰度输出）。
 */

#pragma once

#include "esp_err.h"

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** LVGL 文件系统盘符（大写字母），字体路径形如 "A:NotoSansSC_Regular.ttf" */
#define ESPAPERPLAY_FONTS_DRIVE_LETTER 'A'

/** FreeType 字体样式（与 LVGL lv_freetype_font_style_t 取值一致）。 */
typedef enum {
    ESPAPERPLAY_FONT_STYLE_NORMAL = 0,
    ESPAPERPLAY_FONT_STYLE_ITALIC = 1 << 0,
    ESPAPERPLAY_FONT_STYLE_BOLD = 1 << 1,
} espaperplay_font_style_t;

/**
 * @brief 初始化字体资产。
 *
 * 必须在 LVGL 初始化完成之后调用（esp_lv_fs 注册 LVGL 文件系统驱动；
 * FreeType 引擎由 lv_init() 自动初始化，glyph 缓存数取 Kconfig
 * CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT）。重复调用为幂等操作。
 *
 * @return ESP_OK 成功；否则见 esp_err_t 错误码。
 */
esp_err_t espaperplay_fonts_init(void);

/**
 * @brief 组装字体文件的 LVGL 路径，如 "A:NotoSansSC_Regular.ttf"。
 *
 * @param[in]  file_name 分区内的文件名（如 "NotoSansSC_Regular.ttf"）。
 * @param[out] buf       输出缓冲区。
 * @param[in]  buf_size  缓冲区大小。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE 参数错误。
 */
esp_err_t espaperplay_fonts_get_path(const char *file_name, char *buf, size_t buf_size);

/**
 * @brief 加载（创建）一个 FreeType 字体。
 *
 * 基于 LVGL 的 lv_freetype_font_create()，位图渲染模式，按
 * （文件名, 字号, 样式）做小型缓存：同一参数重复调用返回同一 lv_font_t；
 * 缓存满时淘汰最旧项并销毁对应字体。
 *
 * @note 调用前需已完成 espaperplay_fonts_init()（即 LVGL 已启动）。
 * @note 字体文件内不存在的字符（超出子集范围）渲染为空白/替代字形。
 *
 * @param[in] file_name fonts 分区内的字体文件名。
 * @param[in] size      字号（像素）。
 * @param[in] style     样式（espaperplay_font_style_t 按位组合）。
 * @return 创建的 lv_font_t（调用方持有，可传给 lv_obj_set_style_text_font）；
 *         失败返回 NULL（详见日志）。
 */
lv_font_t *espaperplay_fonts_load(const char *file_name, uint32_t size,
                                  espaperplay_font_style_t style);

#ifdef __cplusplus
}
#endif
