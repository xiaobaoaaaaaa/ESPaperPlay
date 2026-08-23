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
#include "lvgl.h"                /* LV_USE_FREETYPE 开启时内含 lv_freetype.h */
#include "mmap_generate_fonts.h" /* 构建期由 spiffs_create_partition_assets 生成 */

#include "espaperplay_fonts.h"
#include "espaperplay_sd_fonts.h"

static const char *TAG = "ESPaperPlay_FONTS";

static mmap_assets_handle_t s_font_assets;
static esp_lv_fs_handle_t s_fs_handle;

/* ---- FreeType 字体缓存（（文件名, 字号, 样式）→ lv_font_t） ----
 * 容量需容纳全部页面同时活跃的字号档（主界面 / 天气 / 设置 + 模态大字号
 * 合计约 8~9 档）：超限逐出会重建 FreeType face（SD 完整字库的表解析
 * 开销大），页面来回切换时表现为明显卡顿。 */
#define ESPAPERPLAY_FONTS_CACHE_CNT 10

typedef struct {
    char name[64]; /* 请求的字体文件名（缓存键，含来源无关） */
    uint32_t size;
    espaperplay_font_style_t style;
    bool from_sd; /*!< 实际加载来源（日志用） */
    lv_font_t *font;
} font_cache_entry_t;

static font_cache_entry_t s_font_cache[ESPAPERPLAY_FONTS_CACHE_CNT];

/* 当前实际加载（正在使用）的字体名。字体在开机建屏时按 selected_font 加载，
 * 选择后需重启才生效；此处记录实际被渲染的字体（SD 完整字库优先，否则出厂
 * Flash 子集），供 WebUI 判断「当前正在使用」的字体，避免把尚未生效的选择
 * 误判为正在使用。长度与 ESPAPERPLAY_SYSTEM_FONT_NAME_MAX_LEN 保持一致。 */
static char s_active_font_name[64] = ESPAPERPLAY_FONTS_DEFAULT_NAME;

esp_err_t espaperplay_fonts_init(void) {
    if (s_fs_handle != NULL) {
        return ESP_OK; /* 幂等 */
    }

    const mmap_assets_config_t cfg = {
        .partition_label = "fonts",
        .max_files = MMAP_FONTS_FILES,
        .checksum = MMAP_FONTS_CHECKSUM,
        .flags =
            {
                .mmap_enable = 1,
            },
    };

    ESP_RETURN_ON_ERROR(mmap_assets_new(&cfg, &s_font_assets), TAG, "mmap fonts partition failed");

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

    /* 注册 SD 卡完整字体盘符 'B:'（POSIX 代理到 /sdcard/system/fonts）。
     * 失败不阻断：SD 字体仅当卡已挂载且文件存在时才会被选用。 */
    esp_err_t sd_fs_ret = espaperplay_sd_fonts_init();
    if (sd_fs_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD font drive registration failed: %s", esp_err_to_name(sd_fs_ret));
    }

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

    ESP_LOGI(TAG, "fonts ready: %d file(s) on LVGL drive '%c:', FreeType enabled", (int)stored,
             ESPAPERPLAY_FONTS_DRIVE_LETTER);
    return ESP_OK;
}

esp_err_t espaperplay_fonts_get_path(const char *file_name, char *buf, size_t buf_size) {
    if (file_name == NULL || buf == NULL || buf_size < 3) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(buf, buf_size, "%c:%s", ESPAPERPLAY_FONTS_DRIVE_LETTER, file_name);
    if (written < 0 || written >= (int)buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

lv_font_t *espaperplay_fonts_load(const char *file_name, uint32_t size,
                                  espaperplay_font_style_t style) {
    if (file_name == NULL) {
        ESP_LOGE(TAG, "font file name is NULL");
        return NULL;
    }

    /* 先查缓存（键 = 字体名 + 字号 + 样式）：命中即返回，不做 SD 卡存在性
     * 探测。页面建屏时每个标签都会请求字体，若每次都 fopen SD 卡会显著
     * 拖慢加载（设置页一次进入 40+ 个标签）。 */
    for (int i = 0; i < ESPAPERPLAY_FONTS_CACHE_CNT; i++) {
        if (s_font_cache[i].font != NULL && s_font_cache[i].size == size &&
            s_font_cache[i].style == style && strcmp(s_font_cache[i].name, file_name) == 0) {
            return s_font_cache[i].font;
        }
    }

    /* 未命中才选择字体源（优先级从高到低）：
     *   1. SD 卡完整字库（/sdcard/system/fonts/{file_name}，盘符 'B:'）——当卡已挂载
     *      且该文件存在时使用，覆盖 Flash 中的裁剪子集；
     *   2. Flash 字体分区裁剪子集（盘符 'A:'）——回退。 */
    char path[64];
    bool from_sd = false;
    if (espaperplay_sd_fonts_exists(file_name)) {
        if (espaperplay_sd_fonts_get_path(file_name, path, sizeof(path)) == ESP_OK) {
            from_sd = true;
        }
    }

    if (!from_sd) {
        /* Flash 字体分区仅含出厂裁剪子集（默认字体名）。所选字体不在 SD 卡时，
         * 回退到该默认字体，避免加载不存在的字体名导致 FreeType 创建失败。 */
        if (espaperplay_fonts_get_path(ESPAPERPLAY_FONTS_DEFAULT_NAME, path, sizeof(path)) !=
            ESP_OK) {
            ESP_LOGE(TAG, "default font path build failed: %s", ESPAPERPLAY_FONTS_DEFAULT_NAME);
            return NULL;
        }
    }

    /* 记录实际选用的字体名（供 WebUI 判断「当前正在使用」：字体在开机建屏时
     * 加载，选择后需重启才生效，故以实际加载为准而非 NVS 中的 selected_font）。 */
    strlcpy(s_active_font_name, from_sd ? file_name : ESPAPERPLAY_FONTS_DEFAULT_NAME,
            sizeof(s_active_font_name));

    /* 淘汰最旧（线性扫描第一个空位；全满时淘汰第一项） */
    int slot = 0;
    for (int i = 0; i < ESPAPERPLAY_FONTS_CACHE_CNT; i++) {
        if (s_font_cache[i].font == NULL) {
            slot = i;
            break;
        }
    }
    if (s_font_cache[slot].font != NULL) {
        ESP_LOGI(TAG, "font cache full, evicting %s @%u", s_font_cache[slot].name,
                 (unsigned)s_font_cache[slot].size);
        lv_freetype_font_delete(s_font_cache[slot].font);
        s_font_cache[slot].font = NULL;
    }

    lv_font_t *font = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size,
                                              (lv_freetype_font_style_t)style);
    if (font == NULL) {
        ESP_LOGE(TAG, "freetype font create failed: %s @%u (glyphs not in subset render blank)",
                 path, (unsigned)size);
        return NULL;
    }

    strlcpy(s_font_cache[slot].name, from_sd ? file_name : ESPAPERPLAY_FONTS_DEFAULT_NAME,
            sizeof(s_font_cache[slot].name));
    s_font_cache[slot].size = size;
    s_font_cache[slot].style = style;
    s_font_cache[slot].from_sd = from_sd;
    s_font_cache[slot].font = font;
    ESP_LOGI(TAG, "freetype font loaded: %s @%u style=%d (%s)", path, (unsigned)size, (int)style,
             from_sd ? "SD full" : "Flash subset");
    return font;
}

const char *espaperplay_fonts_get_active_name(void) { return s_active_font_name; }
