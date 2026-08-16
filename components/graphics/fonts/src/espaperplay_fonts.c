/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "esp_lv_fs.h"
#include "esp_mmap_assets.h"
#include "lvgl.h" /* LV_USE_FREETYPE 开启时内含 lv_freetype.h */
#include "mmap_generate_fonts.h" /* 构建期由 spiffs_create_partition_assets 生成 */

#include "espaperplay_fonts.h"

static const char *TAG = "ESPaperPlay_FONTS";

static mmap_assets_handle_t s_font_assets;
static esp_lv_fs_handle_t s_fs_handle;

/* ---- FreeType 字体缓存（（文件名, 字号, 样式）→ lv_font_t） ---- */
#define ESPAPERPLAY_FONTS_CACHE_CNT 4

typedef struct {
    char path[64];      /* LVGL 路径，如 "A:NotoSansSC_Regular.ttf" */
    uint32_t size;
    espaperplay_font_style_t style;
    lv_font_t *font;
} font_cache_entry_t;

static font_cache_entry_t s_font_cache[ESPAPERPLAY_FONTS_CACHE_CNT];

esp_err_t espaperplay_fonts_init(void)
{
    if (s_fs_handle != NULL) {
        return ESP_OK; /* 幂等 */
    }

    const mmap_assets_config_t cfg = {
        .partition_label = "fonts",
        .max_files = MMAP_FONTS_FILES,
        .checksum = MMAP_FONTS_CHECKSUM,
        .flags = {
            .mmap_enable = 1,
        },
    };

    ESP_RETURN_ON_ERROR(mmap_assets_new(&cfg, &s_font_assets), TAG,
                        "mmap fonts partition failed");

    const size_t stored = mmap_assets_get_stored_files(s_font_assets);
    ESP_RETURN_ON_FALSE(stored > 0, ESP_ERR_NOT_FOUND, TAG,
                        "no font files found in fonts partition");

    const fs_cfg_t fs_cfg = {
        .fs_letter = ESPAPERPLAY_FONTS_DRIVE_LETTER,
        .fs_nums = (int)stored,
        .fs_assets = s_font_assets,
    };
    ESP_RETURN_ON_ERROR(esp_lv_fs_desc_init(&fs_cfg, &s_fs_handle), TAG,
                        "register LVGL FS drive '%c:' failed", ESPAPERPLAY_FONTS_DRIVE_LETTER);

    /* FreeType 引擎由 lv_init() 自动初始化（LV_USE_FREETYPE 开启时，glyph 缓存数
     * 取 Kconfig CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT），此处无需也不能重复调用。 */

    /* 自检：直接经 LVGL FS 打开第一个字体文件（区分 FS 层与 FreeType 层故障：
     * 此处失败 = 盘符/映射/文件索引问题；此处成功而 lv_freetype_font_create
     * 失败 = FreeType 解析问题）。 */
    const char *first_name = mmap_assets_get_name(s_font_assets, 0);
    if (first_name != NULL) {
        char path[64];
        if (espaperplay_fonts_get_path(first_name, path, sizeof(path)) == ESP_OK) {
            lv_fs_file_t f;
            const lv_fs_res_t fres = lv_fs_open(&f, path, LV_FS_MODE_RD);
            if (fres == LV_FS_RES_OK) {
                uint32_t sz = 0;
                lv_fs_tell(&f, &sz);
                ESP_LOGI(TAG, "selftest: LVGL FS open '%s' OK (%" PRIu32 " bytes)", path, sz);
                lv_fs_close(&f);
            } else {
                ESP_LOGE(TAG, "selftest: LVGL FS open '%s' FAILED res=%d", path, (int)fres);
            }
        }
    }

    ESP_LOGI(TAG, "fonts ready: %d file(s) on LVGL drive '%c:', FreeType enabled",
             (int)stored, ESPAPERPLAY_FONTS_DRIVE_LETTER);
    return ESP_OK;
}

esp_err_t espaperplay_fonts_get_path(const char *file_name, char *buf, size_t buf_size)
{
    if (file_name == NULL || buf == NULL || buf_size < 3) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(buf, buf_size, "%c:%s", ESPAPERPLAY_FONTS_DRIVE_LETTER,
                                 file_name);
    if (written < 0 || written >= (int)buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

lv_font_t *espaperplay_fonts_load(const char *file_name, uint32_t size,
                                  espaperplay_font_style_t style)
{
    char path[sizeof(s_font_cache[0].path)];
    if (espaperplay_fonts_get_path(file_name, path, sizeof(path)) != ESP_OK) {
        ESP_LOGE(TAG, "invalid font file name: %s", file_name ? file_name : "(null)");
        return NULL;
    }

    /* 命中缓存 */
    for (int i = 0; i < ESPAPERPLAY_FONTS_CACHE_CNT; i++) {
        if (s_font_cache[i].font != NULL && s_font_cache[i].size == size &&
            s_font_cache[i].style == style && strcmp(s_font_cache[i].path, path) == 0) {
            return s_font_cache[i].font;
        }
    }

    /* 淘汰最旧（线性扫描第一个空位；全满时淘汰第一项） */
    int slot = 0;
    for (int i = 0; i < ESPAPERPLAY_FONTS_CACHE_CNT; i++) {
        if (s_font_cache[i].font == NULL) {
            slot = i;
            break;
        }
    }
    if (s_font_cache[slot].font != NULL) {
        ESP_LOGI(TAG, "font cache full, evicting %s @%u", s_font_cache[slot].path,
                 (unsigned)s_font_cache[slot].size);
        lv_freetype_font_delete(s_font_cache[slot].font);
        s_font_cache[slot].font = NULL;
    }

    lv_font_t *font = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                              size, (lv_freetype_font_style_t)style);
    if (font == NULL) {
        ESP_LOGE(TAG, "freetype font create failed: %s @%u (glyphs not in subset render blank)",
                 path, (unsigned)size);
        return NULL;
    }

    snprintf(s_font_cache[slot].path, sizeof(s_font_cache[slot].path), "%s", path);
    s_font_cache[slot].size = size;
    s_font_cache[slot].style = style;
    s_font_cache[slot].font = font;
    ESP_LOGI(TAG, "freetype font loaded: %s @%u style=%d", path, (unsigned)size, (int)style);
    return font;
}
