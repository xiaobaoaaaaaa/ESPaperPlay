/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "espaperplay_config.h"
#include "espaperplay_gui.h"

static const char *TAG = "ESPaperPlay_GUI";

/* ====================================================================
 * 帧缓冲尺寸
 * ==================================================================== */

#define GUI_FB_PIXELS (ESPAPERPLAY_DISPLAY_WIDTH * ESPAPERPLAY_DISPLAY_HEIGHT) /* 384000 */
#define GUI_FB_RGB_BYTES (GUI_FB_PIXELS * 2)                                   /* 750KB */
#define GUI_FB_BW_BYTES (GUI_FB_PIXELS / 8)                                    /* 48000 */
#define GUI_FB_GRAY4_BYTES (GUI_FB_PIXELS / 4)                                 /* 96000 */
#define GUI_STRIDE_RGB (ESPAPERPLAY_DISPLAY_WIDTH * 2)                         /* 1600 */

/* ====================================================================
 * 内部状态
 * ==================================================================== */

static uint8_t *s_fb_rgb = NULL;      /*!< RGB565 主帧缓冲 */
static uint8_t *s_stage_bw = NULL;    /*!< 1bpp 转换输出暂存 */
static uint8_t *s_stage_gray4 = NULL; /*!< 2bpp 转换输出暂存 */
static espaperplay_gui_color_t s_color = ESPAPERPLAY_GUI_COLOR_BW;
static espaperplay_gui_converter_t s_converter = ESPAPERPLAY_GUI_CONVERTER_BAYER;
static espaperplay_gui_gray4_dither_t s_gray4_dither = ESPAPERPLAY_GUI_GRAY4_DITHER_FS;
static bool s_initialized = false;

/* 脏区（8 对齐包围盒，一帧内合并） */
static bool s_dirty = false;
static uint16_t s_dirty_x = 0;
static uint16_t s_dirty_y = 0;
static uint16_t s_dirty_w = 0;
static uint16_t s_dirty_h = 0;

/* ====================================================================
 * 颜色转换
 * ==================================================================== */

/**
 * @brief 取 RGB565 像素的 8bit 亮度（Rec.601 加权近似）。
 *
 * RGB565 小端：bit[4:0]=B、bit[10:5]=G、bit[15:11]=R。
 */
static uint8_t gui_luma(const uint8_t *px) {
    const uint16_t p = (uint16_t)px[0] | ((uint16_t)px[1] << 8);
    const uint8_t r = (uint8_t)((p >> 11) & 0x1F);
    const uint8_t g = (uint8_t)((p >> 5) & 0x3F);
    const uint8_t b = (uint8_t)(p & 0x1F);
    /* 565 -> 8bit 放大 */
    const uint16_t r8 = (uint16_t)(r << 3) | (r >> 2);
    const uint16_t g8 = (uint16_t)(g << 2) | (g >> 4);
    const uint16_t b8 = (uint16_t)(b << 3) | (b >> 2);
    return (uint8_t)((r8 * 77 + g8 * 150 + b8 * 29) >> 8);
}

/**
 * @brief Bayer 4x4 有序抖动矩阵（0..15）。
 *
 * 阈值映射 t = m*16+8（范围 8..248）：端点安全——纯黑 0 恒不过阈值、
 * 纯白 255 恒过阈值，纯黑白 UI 位精确、零伪影；中间灰呈 4px 周期点阵。
 */
static const uint8_t gui_bayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

static inline uint8_t gui_bayer_threshold(uint16_t x, uint16_t y) {
    return (uint8_t)(gui_bayer4[((y & 3) << 2) | (x & 3)] * 16 + 8);
}

/**
 * @brief 把 RGB565 主帧的 (x,y,w,h) 区域转换为 1bpp（输出为窗口打包行，
 *        即每行 w/8 字节，与 epd 局部刷新契约一致）。
 *
 * 转换器：THRESHOLD = 快速阈值；BAYER = 4x4 有序抖动，0/255 纯色直通。
 */
static void gui_convert_bw(const uint8_t *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint8_t *out) {
    const uint16_t row_out = w / 8;

    for (uint16_t r = 0; r < h; r++) {
        const uint8_t *row = fb + (size_t)(y + r) * GUI_STRIDE_RGB + (size_t)x * 2;
        uint8_t *dst = out + (size_t)r * row_out;
        for (uint16_t c = 0; c < row_out; c++) {
            uint8_t byte = 0;
            for (int k = 0; k < 8; k++) {
                const uint16_t px = (uint16_t)(c * 8 + k);
                const uint8_t L = gui_luma(row + (size_t)px * 2);
                bool white;
                if (L == 0) {
                    white = false; /* 纯黑直通 */
                } else if (L == 255) {
                    white = true; /* 纯白直通 */
                } else if (s_converter == ESPAPERPLAY_GUI_CONVERTER_THRESHOLD) {
                    white = L >= 128;
                } else {
                    white = L > gui_bayer_threshold((uint16_t)(x + px), (uint16_t)(y + r));
                }
                if (white) {
                    byte |= (uint8_t)(0x80 >> k);
                }
            }
            dst[c] = byte;
        }
    }
}

/**
 * @brief 四灰阶量化：亮度（可含抖动偏置）-> 灰阶值 0..3（0=白 3=黑）。
 */
static inline uint8_t gui_quantize_gray4(int v) {
    uint8_t q; /* 0=最暗（黑）..3=最亮（白） */
    if (v < 42) {
        q = 0;
    } else if (v < 127) {
        q = 1;
    } else if (v < 212) {
        q = 2;
    } else {
        q = 3;
    }
    return (uint8_t)(3 - q); /* 值 0=白 3=黑 */
}

/**
 * @brief 整帧 RGB->2bpp 四灰阶：Bayer 4x4 偏置抖动（快速路径）。
 *
 * 对亮度叠加 ±40 的 Bayer 偏置再量化（4 级边界 42/127/212；纯黑 0/纯白
 * 255 不受偏置影响）。逐像素独立，无行间依赖。
 */
static void gui_convert_gray4_bayer(const uint8_t *fb, uint8_t *out) {
    for (size_t i = 0; i < GUI_FB_PIXELS; i += 4) {
        uint8_t byte = 0;
        for (int k = 0; k < 4; k++) {
            const size_t px = i + k;
            const uint8_t L = gui_luma(fb + px * 2);
            int v = L;
            if (L != 0 && L != 255) {
                v = L + ((int)gui_bayer_threshold((uint16_t)(px % ESPAPERPLAY_DISPLAY_WIDTH),
                                                  (uint16_t)(px / ESPAPERPLAY_DISPLAY_WIDTH)) -
                         128) /
                             3;
            }
            byte |= (uint8_t)(gui_quantize_gray4(v) << (6 - 2 * k));
        }
        out[i / 4] = byte;
    }
}

/**
 * @brief 整帧 RGB->2bpp 四灰阶：Floyd–Steinberg 误差扩散（质量最好）。
 *
 * 误差沿 右 7/16、左下 3/16、下 5/16、右下 1/16 扩散，两行误差缓冲轮换
 * 避免行内污染。逐行依赖误差状态，只适合整帧转换（脏区局部转换会在区域
 * 边界产生误差缺失伪影；灰阶模式恒为全屏刷新，无此问题）。
 */
static void gui_convert_gray4_fs(const uint8_t *fb, uint8_t *out) {
    static int16_t s_err[2][ESPAPERPLAY_DISPLAY_WIDTH + 2];
    int16_t(*cur)[ESPAPERPLAY_DISPLAY_WIDTH + 2] = &s_err[0];
    int16_t(*nxt)[ESPAPERPLAY_DISPLAY_WIDTH + 2] = &s_err[1];

    memset(s_err, 0, sizeof(s_err));
    for (uint16_t y = 0; y < ESPAPERPLAY_DISPLAY_HEIGHT; y++) {
        const uint8_t *row = fb + (size_t)y * GUI_STRIDE_RGB;
        int16_t right = 0; /* 向右扩散（本行，7/16） */
        uint8_t byte = 0;
        for (uint16_t x = 0; x < ESPAPERPLAY_DISPLAY_WIDTH; x++) {
            int v = (int)gui_luma(row + (size_t)x * 2) + right + (*cur)[x];
            if (v < 0) {
                v = 0;
            } else if (v > 255) {
                v = 255;
            }
            /* 量化到 4 级（级别值 0/85/170/255）并计算扩散误差。 */
            const uint8_t q = gui_quantize_gray4(v);
            const int level = (int)(3 - q) * 85; /* 值 0=白 -> 级别 255 */
            const int16_t e = (int16_t)(v - level);
            (*cur)[x] = 0;
            right = (int16_t)((e * 7) >> 4); /* (x+1, y) */
            if (x > 0) {
                (*nxt)[x - 1] = (int16_t)((*nxt)[x - 1] + ((e * 3) >> 4)); /* (x-1, y+1) */
            }
            (*nxt)[x] = (int16_t)((*nxt)[x] + ((e * 5) >> 4));             /* (x, y+1) */
            (*nxt)[x + 1] = (int16_t)((*nxt)[x + 1] + ((e * 1) >> 4));     /* (x+1, y+1) */

            byte |= (uint8_t)(q << (6 - 2 * (x & 3)));
            if ((x & 3) == 3) {
                out[(size_t)y * (ESPAPERPLAY_DISPLAY_WIDTH / 4) + (x >> 2)] = byte;
                byte = 0;
            }
        }
        int16_t(*tmp)[ESPAPERPLAY_DISPLAY_WIDTH + 2] = cur;
        cur = nxt;
        nxt = tmp;
        memset(*nxt, 0, sizeof(*nxt));
    }
}

/**
 * @brief 整帧 RGB->2bpp 四灰阶转换（按当前抖动算法分发）。
 */
static void gui_convert_gray4(const uint8_t *fb, uint8_t *out) {
    if (s_gray4_dither == ESPAPERPLAY_GUI_GRAY4_DITHER_FS) {
        gui_convert_gray4_fs(fb, out);
    } else {
        gui_convert_gray4_bayer(fb, out);
    }
}

/* ====================================================================
 * 内部工具
 * ==================================================================== */

static uint8_t *gui_alloc(size_t bytes) {
    uint8_t *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return p;
}

/* ====================================================================
 * 自检任务（仅 ESPAPERPLAY_GUI_ENABLE_SELFTEST=1 时启用）
 * ==================================================================== */

#if ESPAPERPLAY_GUI_ENABLE_SELFTEST
static void gui_test_pixel(uint8_t *fb, uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t p = (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3));
    uint8_t *px = fb + (size_t)y * GUI_STRIDE_RGB + (size_t)x * 2;
    px[0] = (uint8_t)(p & 0xFF);
    px[1] = (uint8_t)(p >> 8);
}

static void gui_test_fill(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t r,
                          uint8_t g, uint8_t b) {
    for (uint16_t j = 0; j < h; j++) {
        for (uint16_t i = 0; i < w; i++) {
            gui_test_pixel(fb, (uint16_t)(x + i), (uint16_t)(y + j), r, g, b);
        }
    }
}

/** 水平亮度渐变（黑->白），亮度 = 列位置比例。 */
static void gui_test_gradient(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    for (uint16_t j = 0; j < h; j++) {
        for (uint16_t i = 0; i < w; i++) {
            const uint8_t L = (uint8_t)((uint32_t)i * 255 / (w - 1));
            gui_test_pixel(fb, (uint16_t)(x + i), (uint16_t)(y + j), L, L, L);
        }
    }
}

/**
 * @brief 渲染后端自检：
 *  1. 清白；
 *  2. 纯黑白 UI 位精确性（BAYER）：白底 + 黑框 + 黑方块，交界应无点状伪影；
 *  3. 灰度渐变（BAYER）：局部刷新，应呈平滑点阵（无大色块/色带）；
 *  4. 阈值对照：同渐变用 THRESHOLD 局部刷新，应见明显色带（与 3 对比）；
 *  5. 转换性能：整帧 BW（阈值/Bayer）与 gray4（Bayer/FS）转换耗时；
 *  6. gray4 高清（FS）：黑/深灰/浅灰/白四带 + 渐变，全屏四灰阶刷新；
 *  7. gray4 高清（Bayer）：同内容再刷一次，与 6 对比点阵纹理差异；
 *  8. 模式切换残留：主帧置全白后 show_ui，面板应纯白（灰阶中间灰被清除）。
 */
static void gui_selftest_task(void *arg) {
    (void)arg;
    espaperplay_gui_framebuffer_t fb;
    esp_err_t ret;

    ESP_LOGI(TAG, "GUI backend selftest started (RGB565 + converters)");
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 1. 清白。 */
    ret = espaperplay_gui_clear();
    ESP_LOGI(TAG, "selftest: clear -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 2. 纯黑白 UI（BAYER 完整性）：白底黑框 + 中央黑方块。 */
    ret = espaperplay_gui_get_framebuffer(&fb);
    if (ret != ESP_OK) {
        goto out;
    }
    gui_test_fill(fb.buffer, 8, 8, 784, 16, 0, 0, 0);       /* 上边框 */
    gui_test_fill(fb.buffer, 8, 456, 784, 16, 0, 0, 0);     /* 下边框 */
    gui_test_fill(fb.buffer, 8, 8, 16, 464, 0, 0, 0);       /* 左边框 */
    gui_test_fill(fb.buffer, 776, 8, 16, 464, 0, 0, 0);     /* 右边框 */
    gui_test_fill(fb.buffer, 360, 190, 80, 80, 0, 0, 0);    /* 中央黑方块 */
    espaperplay_gui_submit_area(0, 0, fb.width, fb.height);
    ret = espaperplay_gui_flush();
    ESP_LOGI(TAG, "selftest: bw pure-black UI (bayer) -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 3. 灰度渐变（BAYER 局部刷新）。 */
    gui_test_gradient(fb.buffer, 200, 60, 200, 120);
    espaperplay_gui_submit_area(200, 60, 200, 120);
    ret = espaperplay_gui_flush();
    ESP_LOGI(TAG, "selftest: gradient bayer (partial) -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 4. 阈值对照：同一渐变（新区域）用 THRESHOLD。 */
    gui_test_gradient(fb.buffer, 440, 60, 160, 120);
    ret = espaperplay_gui_set_converter(ESPAPERPLAY_GUI_CONVERTER_THRESHOLD);
    if (ret != ESP_OK) {
        goto out;
    }
    espaperplay_gui_submit_area(440, 60, 160, 120);
    ret = espaperplay_gui_flush();
    ESP_LOGI(TAG, "selftest: gradient threshold (partial) -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    espaperplay_gui_set_converter(ESPAPERPLAY_GUI_CONVERTER_BAYER); /* 恢复默认 */

    /* 5. 转换性能：整帧四种转换路径耗时（内容无关，直接调用转换器）。 */
    {
        int64_t t0;
        t0 = esp_timer_get_time();
        gui_convert_bw(fb.buffer, 0, 0, ESPAPERPLAY_DISPLAY_WIDTH, ESPAPERPLAY_DISPLAY_HEIGHT,
                       s_stage_bw);
        ESP_LOGI(TAG, "perf: bw bayer full convert -> %lld ms", (esp_timer_get_time() - t0) / 1000);
        s_converter = ESPAPERPLAY_GUI_CONVERTER_THRESHOLD;
        t0 = esp_timer_get_time();
        gui_convert_bw(fb.buffer, 0, 0, ESPAPERPLAY_DISPLAY_WIDTH, ESPAPERPLAY_DISPLAY_HEIGHT,
                       s_stage_bw);
        ESP_LOGI(TAG, "perf: bw threshold full convert -> %lld ms",
                 (esp_timer_get_time() - t0) / 1000);
        s_converter = ESPAPERPLAY_GUI_CONVERTER_BAYER;
        s_gray4_dither = ESPAPERPLAY_GUI_GRAY4_DITHER_BAYER;
        t0 = esp_timer_get_time();
        gui_convert_gray4(s_fb_rgb, s_stage_gray4);
        ESP_LOGI(TAG, "perf: gray4 bayer full convert -> %lld ms",
                 (esp_timer_get_time() - t0) / 1000);
        s_gray4_dither = ESPAPERPLAY_GUI_GRAY4_DITHER_FS;
        t0 = esp_timer_get_time();
        gui_convert_gray4(s_fb_rgb, s_stage_gray4);
        ESP_LOGI(TAG, "perf: gray4 floyd-steinberg full convert -> %lld ms",
                 (esp_timer_get_time() - t0) / 1000);
    }

    /* 6. gray4 高清（FS，默认）：黑/深灰/浅灰/白四带 + 中段渐变。 */
    gui_test_fill(fb.buffer, 0, 0, 200, 480, 0, 0, 0);       /* 黑带 */
    gui_test_fill(fb.buffer, 200, 0, 200, 480, 85, 85, 85);  /* 深灰带 */
    gui_test_gradient(fb.buffer, 400, 0, 200, 480);          /* 渐变带 */
    gui_test_fill(fb.buffer, 600, 0, 200, 480, 255, 255, 255); /* 白带 */
    ret = espaperplay_gui_show_image();
    ESP_LOGI(TAG, "selftest: gray4 full (FS) -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 7. gray4 高清（Bayer）：同内容再刷一次，与 FS 对比点阵纹理。 */
    ret = espaperplay_gui_set_gray4_dither(ESPAPERPLAY_GUI_GRAY4_DITHER_BAYER);
    if (ret != ESP_OK) {
        goto out;
    }
    ret = espaperplay_gui_show_image();
    ESP_LOGI(TAG, "selftest: gray4 full (Bayer) -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    espaperplay_gui_set_gray4_dither(ESPAPERPLAY_GUI_GRAY4_DITHER_FS); /* 恢复默认 */

    /* 8. 模式切换残留：主帧置全白后切回交互模式，面板应纯白。 */
    memset(fb.buffer, 0xFF, GUI_FB_RGB_BYTES); /* RGB565 白 */
    ret = espaperplay_gui_show_ui();
    ESP_LOGI(TAG, "selftest: show_ui after gray4 (residue check) -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

out:
    ret = espaperplay_gui_clear();
    ESP_LOGI(TAG, "GUI selftest done, clear -> %s", esp_err_to_name(ret));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
#endif /* ESPAPERPLAY_GUI_ENABLE_SELFTEST */

/* ====================================================================
 * 公共 API
 * ==================================================================== */

esp_err_t espaperplay_gui_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    s_fb_rgb = gui_alloc(GUI_FB_RGB_BYTES);
    s_stage_bw = gui_alloc(GUI_FB_BW_BYTES);
    s_stage_gray4 = gui_alloc(GUI_FB_GRAY4_BYTES);
    if (s_fb_rgb == NULL || s_stage_bw == NULL || s_stage_gray4 == NULL) {
        ESP_LOGE(TAG, "alloc failed (rgb=%u bw=%u gray4=%u)", (unsigned)GUI_FB_RGB_BYTES,
                 (unsigned)GUI_FB_BW_BYTES, (unsigned)GUI_FB_GRAY4_BYTES);
        heap_caps_free(s_fb_rgb);
        heap_caps_free(s_stage_bw);
        heap_caps_free(s_stage_gray4);
        s_fb_rgb = s_stage_bw = s_stage_gray4 = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb_rgb, 0xFF, GUI_FB_RGB_BYTES); /* 默认全白（RGB565 0xFFFF） */
    s_color = ESPAPERPLAY_GUI_COLOR_BW;
    s_converter = ESPAPERPLAY_GUI_CONVERTER_BAYER;
    s_gray4_dither = ESPAPERPLAY_GUI_GRAY4_DITHER_FS;
    s_dirty = false;
    s_initialized = true;

    ESP_LOGI(TAG, "GUI backend ready: rgb %uB + stage bw %uB + gray4 %uB (PSRAM preferred)",
             (unsigned)GUI_FB_RGB_BYTES, (unsigned)GUI_FB_BW_BYTES,
             (unsigned)GUI_FB_GRAY4_BYTES);

#if ESPAPERPLAY_GUI_ENABLE_SELFTEST
    xTaskCreate(gui_selftest_task, "gui_selftest", 4096, NULL, 5, NULL);
#endif /* ESPAPERPLAY_GUI_ENABLE_SELFTEST */

    return ESP_OK;
}

esp_err_t espaperplay_gui_start(void) {
    ESP_LOGW(TAG, "gui_start not implemented yet (LVGL task pending)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espaperplay_gui_set_color(espaperplay_gui_color_t color) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (color >= ESPAPERPLAY_GUI_COLOR_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (color != s_color) {
        ESP_LOGI(TAG, "color mode: %s", color == ESPAPERPLAY_GUI_COLOR_GRAY4 ? "gray4" : "bw");
        s_color = color;
        s_dirty = false; /* 旧脏区不再有效，等待重新提交 */
    }
    return ESP_OK;
}

esp_err_t espaperplay_gui_set_converter(espaperplay_gui_converter_t converter) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (converter >= ESPAPERPLAY_GUI_CONVERTER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (converter != s_converter) {
        ESP_LOGI(TAG, "converter: %s",
                 converter == ESPAPERPLAY_GUI_CONVERTER_BAYER ? "bayer4x4" : "threshold");
        s_converter = converter;
    }
    return ESP_OK;
}

esp_err_t espaperplay_gui_set_gray4_dither(espaperplay_gui_gray4_dither_t dither) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dither >= ESPAPERPLAY_GUI_GRAY4_DITHER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dither != s_gray4_dither) {
        ESP_LOGI(TAG, "gray4 dither: %s",
                 dither == ESPAPERPLAY_GUI_GRAY4_DITHER_FS ? "floyd-steinberg" : "bayer");
        s_gray4_dither = dither;
    }
    return ESP_OK;
}

esp_err_t espaperplay_gui_get_framebuffer(espaperplay_gui_framebuffer_t *fb) {
    if (fb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    fb->buffer = s_fb_rgb;
    fb->width = ESPAPERPLAY_DISPLAY_WIDTH;
    fb->height = ESPAPERPLAY_DISPLAY_HEIGHT;
    fb->stride = GUI_STRIDE_RGB;
    fb->color = s_color;
    return ESP_OK;
}

esp_err_t espaperplay_gui_submit_area(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    uint16_t xa, x_end, wa;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width == 0 || height == 0) {
        return ESP_OK; /* 空区域忽略 */
    }
    /* 裁剪到屏内。 */
    if (x >= ESPAPERPLAY_DISPLAY_WIDTH || y >= ESPAPERPLAY_DISPLAY_HEIGHT) {
        return ESP_OK;
    }
    if ((uint32_t)x + width > ESPAPERPLAY_DISPLAY_WIDTH) {
        width = ESPAPERPLAY_DISPLAY_WIDTH - x;
    }
    if ((uint32_t)y + height > ESPAPERPLAY_DISPLAY_HEIGHT) {
        height = ESPAPERPLAY_DISPLAY_HEIGHT - y;
    }
    /* X 方向 8 像素对齐（驱动局部刷新要求）：左边界向下取整、右边界向上取整。 */
    xa = x & (uint16_t)~7u;
    x_end = (uint16_t)((x + width + 7) & ~7u);
    if (x_end > ESPAPERPLAY_DISPLAY_WIDTH) {
        x_end = ESPAPERPLAY_DISPLAY_WIDTH;
    }
    wa = x_end - xa;

    /* 合并到脏区包围盒。 */
    if (!s_dirty) {
        s_dirty_x = xa;
        s_dirty_y = y;
        s_dirty_w = wa;
        s_dirty_h = height;
        s_dirty = true;
    } else {
        const uint16_t x0 = s_dirty_x < xa ? s_dirty_x : xa;
        const uint16_t y0 = s_dirty_y < y ? s_dirty_y : y;
        const uint16_t x1 = (uint16_t)(s_dirty_x + s_dirty_w) > x_end
                                ? (uint16_t)(s_dirty_x + s_dirty_w)
                                : x_end;
        const uint16_t y1 = (uint16_t)(s_dirty_y + s_dirty_h) > (uint16_t)(y + height)
                                ? (uint16_t)(s_dirty_y + s_dirty_h)
                                : (uint16_t)(y + height);
        s_dirty_x = x0;
        s_dirty_y = y0;
        s_dirty_w = (uint16_t)(x1 - x0);
        s_dirty_h = (uint16_t)(y1 - y0);
    }
    return ESP_OK;
}

esp_err_t espaperplay_gui_flush(void) {
    esp_err_t ret;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_dirty) {
        return ESP_OK;
    }

    if (s_color == ESPAPERPLAY_GUI_COLOR_GRAY4) {
        /* 高清模式：整帧 RGB->2bpp（Bayer 偏置抖动），全屏刷新（驱动约束）。 */
        gui_convert_gray4(s_fb_rgb, s_stage_gray4);
        ret = espaperplay_epd_refresh(s_stage_gray4, 0, 0, 0, 0, ESPAPERPLAY_EPD_MODE_GRAY4);
    } else if ((uint32_t)s_dirty_w * s_dirty_h >= ESPAPERPLAY_GUI_FULL_AREA_THRESHOLD_PIXELS) {
        /* 大面积：整帧 RGB->1bpp，全屏刷新。 */
        gui_convert_bw(s_fb_rgb, 0, 0, ESPAPERPLAY_DISPLAY_WIDTH, ESPAPERPLAY_DISPLAY_HEIGHT,
                       s_stage_bw);
        ret = espaperplay_epd_refresh(s_stage_bw, 0, 0, 0, 0, ESPAPERPLAY_EPD_MODE_FULL);
    } else {
        /* 小区域：只转换脏区（输出即窗口打包行），局部刷新。 */
        gui_convert_bw(s_fb_rgb, s_dirty_x, s_dirty_y, s_dirty_w, s_dirty_h, s_stage_bw);
        ret = espaperplay_epd_refresh(s_stage_bw, s_dirty_x, s_dirty_y, s_dirty_w, s_dirty_h,
                                      ESPAPERPLAY_EPD_MODE_PARTIAL);
    }

    if (ret == ESP_OK) {
        s_dirty = false;
    } else {
        ESP_LOGE(TAG, "flush failed: %s (dirty area kept for retry)", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t espaperplay_gui_show_ui(void) {
    esp_err_t ret = espaperplay_gui_set_color(ESPAPERPLAY_GUI_COLOR_BW);
    if (ret != ESP_OK) {
        return ret;
    }
    espaperplay_gui_submit_area(0, 0, ESPAPERPLAY_DISPLAY_WIDTH, ESPAPERPLAY_DISPLAY_HEIGHT);
    return espaperplay_gui_flush();
}

esp_err_t espaperplay_gui_show_image(void) {
    esp_err_t ret = espaperplay_gui_set_color(ESPAPERPLAY_GUI_COLOR_GRAY4);
    if (ret != ESP_OK) {
        return ret;
    }
    espaperplay_gui_submit_area(0, 0, ESPAPERPLAY_DISPLAY_WIDTH, ESPAPERPLAY_DISPLAY_HEIGHT);
    return espaperplay_gui_flush();
}

esp_err_t espaperplay_gui_clear(void) {
    esp_err_t ret;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_fb_rgb, 0xFF, GUI_FB_RGB_BYTES); /* RGB565 白 */
    ret = (s_color == ESPAPERPLAY_GUI_COLOR_GRAY4)
              ? espaperplay_epd_refresh(NULL, 0, 0, 0, 0, ESPAPERPLAY_EPD_MODE_GRAY4)
              : espaperplay_epd_refresh(NULL, 0, 0, 0, 0, ESPAPERPLAY_EPD_MODE_FULL);
    if (ret == ESP_OK) {
        s_dirty = false;
    }
    return ret;
}
