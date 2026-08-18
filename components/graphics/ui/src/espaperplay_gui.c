/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "espaperplay_config.h"
#include "espaperplay_display.h"
#include "espaperplay_gui.h"

static const char *TAG = "ESPaperPlay_GUI";

/* ====================================================================
 * 帧缓冲尺寸
 * ==================================================================== */

/* 显示参数（运行时，来自 espaperplay_display；gui_init 时读取） */
static uint16_t s_disp_w = 0; /*!< 显示区宽度（像素） */
static uint16_t s_disp_h = 0; /*!< 显示区高度（像素） */
static uint32_t s_fb_pixels = 0;      /*!< 全屏像素数 w*h */
static size_t s_fb_rgb_bytes = 0;     /*!< RGB565 主帧字节数 w*h*2 */
static size_t s_fb_bw_bytes = 0;      /*!< 1bpp 帧字节数 w*h/8 */
static size_t s_fb_gray4_bytes = 0;   /*!< 2bpp 帧字节数 w*h/4 */
static size_t s_stride_rgb = 0;       /*!< 主帧行字节数 w*2 */
static uint32_t s_full_threshold = 0; /*!< 全屏阈值（70% 像素，运行时） */

/* ====================================================================
 * 内部状态
 * ==================================================================== */

static uint8_t *s_fb_rgb = NULL; /*!< RGB565 主帧缓冲 */
/* 快照暂存（双槽位：A=优先处理，B=排队）——flush 时把主帧内容冻结进暂存，
 * 之后渲染器可继续修改主帧而不影响已排队帧。 */
static uint8_t *s_stage_a_bw = NULL;    /*!< 槽 A 的 1bpp 快照 */
static uint8_t *s_stage_b_bw = NULL;    /*!< 槽 B 的 1bpp 快照 */
static uint8_t *s_stage_a_gray4 = NULL; /*!< 槽 A 的 2bpp 快照 */
static uint8_t *s_stage_b_gray4 = NULL; /*!< 槽 B 的 2bpp 快照 */
static espaperplay_gui_color_t s_color = ESPAPERPLAY_GUI_COLOR_BW;
static espaperplay_gui_converter_t s_converter = ESPAPERPLAY_GUI_CONVERTER_THRESHOLD;
static espaperplay_gui_gray4_dither_t s_gray4_dither = ESPAPERPLAY_GUI_GRAY4_DITHER_FS;
static bool s_initialized = false;

static SemaphoreHandle_t s_lock = NULL;   /*!< 槽位/脏区状态互斥（渲染任务 vs worker） */
static TaskHandle_t s_worker_task = NULL; /*!< 异步刷新 worker（NULL=退化同步） */

/* 脏区（8 对齐包围盒，一帧内合并；由渲染任务在锁内读写） */
static bool s_dirty = false;
static uint16_t s_dirty_x = 0;
static uint16_t s_dirty_y = 0;
static uint16_t s_dirty_w = 0;
static uint16_t s_dirty_h = 0;

/* 大面积局刷计数：脏区面积超过全屏阈值时不立即全刷，而是继续局刷并计数；
 * 连续达到 s_full_force_after 次才强制一次全像素翻转全刷，清除累积残影。
 * 计数在 worker 出队执行时递增（按"已执行"而非"已快照"），杜绝连刷。 */
static uint32_t s_large_partial_count = 0;
/* 连续大面积局刷后强制全刷的阈值（0=禁用；运行期可经 Web 调整并持久化）。 */
static uint32_t s_full_force_after = ESPAPERPLAY_GUI_FULL_FORCE_AFTER;

/* 刷新操作槽位状态：
 *   IDLE  = 空闲，可被渲染端快照写入；
 *   READY = 已快照、待 worker 取走（可被渲染端并入重快照取最新帧）；
 *   BUSY  = worker 正在执行（stage 缓冲正被 SPI 读取，渲染端严禁触碰）。 */
typedef enum { GUI_SLOT_IDLE = 0, GUI_SLOT_READY, GUI_SLOT_BUSY } gui_slot_state_t;

/* 刷新操作槽位 */
typedef enum { GUI_OP_FRAME = 0, GUI_OP_CLEAR } gui_op_type_t;
typedef struct {
    gui_slot_state_t state;        /*!< 槽位状态（IDLE/READY/BUSY） */
    gui_op_type_t type;            /*!< FRAME=快照帧；CLEAR=深擦除清白 */
    espaperplay_gui_color_t color; /*!< 快照时的模式 */
    uint16_t x, y, w, h;           /*!< FRAME 有效（BW 局部窗口；全屏/灰阶=全帧） */
    bool large_area;               /*!< 脏区是否达大面积阈值（BW 计数用） */
    bool force_full;               /*!< 强制全像素翻转全刷（清残影） */
    const uint8_t *stage;          /*!< 已转换的快照缓冲（CLEAR 为 NULL） */
} gui_op_t;
static gui_op_t s_op_a; /*!< 槽 A：worker 优先处理 */
static gui_op_t s_op_b; /*!< 槽 B：排队（槽满时新脏区并入 READY 的 FRAME 槽重快照） */

/* ====================================================================
 * 颜色转换（RGB565 -> 亮度 -> 1bpp / 2bpp）
 * ==================================================================== */

/* 亮度（Rec.601 加权近似）。查表优化实测仅提速 ~7%（-Og 下瓶颈是循环
 * 开销而非乘法），收益不明显，维持直接计算版本。 */
static inline uint8_t gui_luma(const uint8_t *px) {
    const uint16_t p = (uint16_t)px[0] | ((uint16_t)px[1] << 8);
    const uint8_t r = (uint8_t)((p >> 11) & 0x1F);
    const uint8_t g = (uint8_t)((p >> 5) & 0x3F);
    const uint8_t b = (uint8_t)(p & 0x1F);
    const uint16_t r8 = (uint16_t)(r << 3) | (r >> 2);
    const uint16_t g8 = (uint16_t)(g << 2) | (g >> 4);
    const uint16_t b8 = (uint16_t)(b << 3) | (b >> 2);
    return (uint8_t)((r8 * 77 + g8 * 150 + b8 * 29) >> 8);
}

static const uint8_t gui_bayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

static inline uint8_t gui_bayer_threshold(uint16_t x, uint16_t y) {
    return (uint8_t)(gui_bayer4[((y & 3) << 2) | (x & 3)] * 16 + 8);
}

static void gui_convert_bw(const uint8_t *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint8_t *out) {
    const uint16_t row_out = w / 8;

    for (uint16_t r = 0; r < h; r++) {
        const uint8_t *row = fb + (size_t)(y + r) * s_stride_rgb + (size_t)x * 2;
        uint8_t *dst = out + (size_t)r * row_out;
        for (uint16_t c = 0; c < row_out; c++) {
            uint8_t byte = 0;
            for (int k = 0; k < 8; k++) {
                const uint16_t px = (uint16_t)(c * 8 + k);
                const uint8_t L = gui_luma(row + (size_t)px * 2);
                bool white;
                if (L == 0) {
                    white = false;
                } else if (L == 255) {
                    white = true;
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

static inline uint8_t gui_quantize_gray4(int v) {
    uint8_t q;
    if (v < 42) {
        q = 0;
    } else if (v < 127) {
        q = 1;
    } else if (v < 212) {
        q = 2;
    } else {
        q = 3;
    }
    return (uint8_t)(3 - q);
}

static void gui_convert_gray4_bayer(const uint8_t *fb, uint8_t *out) {
    for (size_t i = 0; i < s_fb_pixels; i += 4) {
        uint8_t byte = 0;
        for (int k = 0; k < 4; k++) {
            const size_t px = i + k;
            const uint8_t L = gui_luma(fb + px * 2);
            int v = L;
            if (L != 0 && L != 255) {
                v = L + ((int)gui_bayer_threshold((uint16_t)(px % s_disp_w),
                                                  (uint16_t)(px / s_disp_w)) -
                         128) /
                            3;
            }
            byte |= (uint8_t)(gui_quantize_gray4(v) << (6 - 2 * k));
        }
        out[i / 4] = byte;
    }
}

/* Floyd-Steinberg 误差缓冲（动态，gui_init 按分辨率分配：2 行 x (w+2)）。 */
static int16_t *s_fs_err = NULL;

static void gui_convert_gray4_fs(const uint8_t *fb, uint8_t *out) {
    /* 布局：[row] 行内 x 偏移 +1（左右各留 1 列）。 */
    const size_t stride = (size_t)s_disp_w + 2;
    int16_t *cur = s_fs_err;
    int16_t *nxt = s_fs_err + stride;

    memset(s_fs_err, 0, stride * 2 * sizeof(int16_t));
    for (uint16_t y = 0; y < s_disp_h; y++) {
        const uint8_t *row = fb + (size_t)y * s_stride_rgb;
        int16_t right = 0;
        uint8_t byte = 0;
        for (uint16_t x = 0; x < s_disp_w; x++) {
            int v = (int)gui_luma(row + (size_t)x * 2) + right + cur[x + 1];
            if (v < 0) {
                v = 0;
            } else if (v > 255) {
                v = 255;
            }
            const uint8_t q = gui_quantize_gray4(v);
            const int level = (int)(3 - q) * 85;
            const int16_t e = (int16_t)(v - level);
            cur[x + 1] = 0;
            right = (int16_t)((e * 7) >> 4);
            if (x > 0) {
                nxt[x] = (int16_t)(nxt[x] + ((e * 3) >> 4));
            }
            nxt[x + 1] = (int16_t)(nxt[x + 1] + ((e * 5) >> 4));
            nxt[x + 2] = (int16_t)(nxt[x + 2] + ((e * 1) >> 4));

            byte |= (uint8_t)(q << (6 - 2 * (x & 3)));
            if ((x & 3) == 3) {
                out[(size_t)y * (s_disp_w / 4) + (x >> 2)] = byte;
                byte = 0;
            }
        }
        int16_t *tmp = cur;
        cur = nxt;
        nxt = tmp;
        memset(nxt, 0, stride * sizeof(int16_t));
    }
}

static void gui_convert_gray4(const uint8_t *fb, uint8_t *out) {
    if (s_gray4_dither == ESPAPERPLAY_GUI_GRAY4_DITHER_FS) {
        gui_convert_gray4_fs(fb, out);
    } else {
        gui_convert_gray4_bayer(fb, out);
    }
}

/* ====================================================================
 * 快照与 worker
 * ==================================================================== */

/**
 * @brief 把当前脏区按当前模式快照转换进指定暂存（调用方须持锁）。
 *
 * BW 全屏策略：脏区面积超过阈值时*不*立即全刷——继续按包围盒局刷。
 * 是否强制全像素翻转全刷（op->force_full）由"已执行的大面积局刷计数"
 * s_large_partial_count 决定（该计数在 worker 出队时递增，见
 * gui_count_executed）；本函数只读计数，不做累加/清零。
 */
static void gui_snapshot(gui_op_t *op, uint8_t *stage_bw, uint8_t *stage_gray4) {
    op->state = GUI_SLOT_READY;
    op->color = s_color;
    if (s_color == ESPAPERPLAY_GUI_COLOR_GRAY4) {
        gui_convert_gray4(s_fb_rgb, stage_gray4);
        op->type = GUI_OP_FRAME;
        op->stage = stage_gray4;
        op->x = 0;
        op->y = 0;
        op->w = s_disp_w;
        op->h = s_disp_h;
        op->large_area = false;
        op->force_full = false;
    } else {
        const uint32_t area = (uint32_t)s_dirty_w * s_dirty_h;
        const bool large = area >= s_full_threshold;
        const bool do_force =
            large && s_full_force_after > 0 && s_large_partial_count >= s_full_force_after;

        op->type = GUI_OP_FRAME;
        op->stage = stage_bw;
        op->large_area = large;
        if (do_force) {
            /* 连续大面积局刷已执行满阈值：本次强制全像素翻转全刷清残影。 */
            gui_convert_bw(s_fb_rgb, 0, 0, s_disp_w, s_disp_h,
                           stage_bw);
            op->force_full = true;
            op->x = 0;
            op->y = 0;
            op->w = s_disp_w;
            op->h = s_disp_h;
        } else {
            gui_convert_bw(s_fb_rgb, s_dirty_x, s_dirty_y, s_dirty_w, s_dirty_h, stage_bw);
            op->force_full = false;
            op->x = s_dirty_x;
            op->y = s_dirty_y;
            op->w = s_dirty_w;
            op->h = s_dirty_h;
        }
    }
}

/**
 * @brief 执行一个已快照的刷新操作（阻塞；worker 或同步退化路径调用）。
 */
static esp_err_t gui_execute_op(const gui_op_t *op) {
    esp_err_t ret;

    /* 数据安全由槽位状态机保证：本 op 所在槽在 worker 执行期间为 BUSY，
     * 渲染端不会重写其 stage 缓冲；epd 驱动内部亦会快照，双重防护。 */
    if (op->type == GUI_OP_CLEAR) {
        ret = (op->color == ESPAPERPLAY_GUI_COLOR_GRAY4)
                  ? espaperplay_epd_refresh(NULL, 0, 0, 0, 0, ESPAPERPLAY_EPD_MODE_GRAY4)
                  : espaperplay_epd_refresh(NULL, 0, 0, 0, 0, ESPAPERPLAY_EPD_MODE_FULL);
    } else if (op->color == ESPAPERPLAY_GUI_COLOR_GRAY4) {
        ret = espaperplay_epd_refresh(op->stage, 0, 0, 0, 0, ESPAPERPLAY_EPD_MODE_GRAY4);
    } else if (op->force_full) {
        /* 周期性强制全刷：全像素翻转深波形，清除局刷累积残影。 */
        ret = espaperplay_epd_refresh(op->stage, 0, 0, 0, 0, ESPAPERPLAY_EPD_MODE_FULL_FORCE);
    } else {
        /* 普通局部刷新（窗口 = 脏区包围盒，差分 N2OCP）。 */
        ret = espaperplay_epd_refresh(op->stage, op->x, op->y, op->w, op->h,
                                      ESPAPERPLAY_EPD_MODE_PARTIAL);
    }
    return ret;
}

/**
 * @brief 按"已执行"更新大面积局刷计数（worker 出队 / 同步执行前调用，须持锁）。
 *
 * 只有 BW 普通局刷参与计数：大面积 +1、小面积清零；force / clear / gray4
 * 都是全屏刷新，会建立新基线，计数清零。force 在出队那一刻即清零，因此
 * force 执行期间渲染端合并进另一槽的帧只会计数，不会再排队下一个 force。
 */
static void gui_count_executed(const gui_op_t *op) {
    if (op->type == GUI_OP_FRAME && op->color == ESPAPERPLAY_GUI_COLOR_BW && !op->force_full) {
        if (op->large_area && s_full_force_after > 0) {
            if (s_large_partial_count < s_full_force_after) {
                s_large_partial_count++;
            }
        } else {
            s_large_partial_count = 0;
        }
    } else {
        s_large_partial_count = 0;
    }
}

/**
 * @brief 异步刷新 worker 任务。
 *
 * 阻塞（SPI 传输 + 等 BUSY）只发生在本任务，渲染/调用线程永不被刷新阻塞。
 * 任务通知计数驱动：每次排队都会通知一次；空转时阻塞等待。
 */
static void gui_worker_task(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "refresh worker started");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        gui_op_t op;
        gui_op_t *slot = NULL;
        if (s_op_a.state == GUI_SLOT_READY) {
            slot = &s_op_a;
        } else if (s_op_b.state == GUI_SLOT_READY) {
            slot = &s_op_b;
        }
        if (slot != NULL) {
            op = *slot;
            slot->state = GUI_SLOT_BUSY; /* 占用：渲染端不得再写该槽 */
            gui_count_executed(&op);     /* 按"已执行"计数（force 出队即清零） */
        }
        xSemaphoreGive(s_lock);
        if (slot == NULL) {
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        const esp_err_t ret = gui_execute_op(&op);
        if (op.type == GUI_OP_CLEAR) {
            ESP_LOGI(TAG, "worker: clear -> %s (%lld ms)", esp_err_to_name(ret),
                     (esp_timer_get_time() - t0) / 1000);
        } else if (op.force_full) {
            ESP_LOGI(TAG, "worker: FULL! 800x480 (force) -> %s (%lld ms)", esp_err_to_name(ret),
                     (esp_timer_get_time() - t0) / 1000);
        } else if (op.color == ESPAPERPLAY_GUI_COLOR_GRAY4) {
            ESP_LOGI(TAG, "worker: G4 800x480 -> %s (%lld ms)", esp_err_to_name(ret),
                     (esp_timer_get_time() - t0) / 1000);
        } else {
            ESP_LOGI(TAG, "worker: PART %ux%u@(%u,%u) -> %s (%lld ms)", op.w, op.h, op.x, op.y,
                     esp_err_to_name(ret), (esp_timer_get_time() - t0) / 1000);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "worker: refresh failed: %s", esp_err_to_name(ret));
        }

        /* 执行完毕：释放槽位，渲染端可再次快照写入。 */
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            slot->state = GUI_SLOT_IDLE;
            xSemaphoreGive(s_lock);
        }
    }
}

/* ====================================================================
 * 自检任务（仅 ESPAPERPLAY_GUI_ENABLE_SELFTEST=1 时启用）
 * ==================================================================== */

#if ESPAPERPLAY_GUI_ENABLE_SELFTEST
static void gui_test_pixel(uint8_t *fb, uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t p =
        (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3));
    uint8_t *px = fb + (size_t)y * s_stride_rgb + (size_t)x * 2;
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

static void gui_test_gradient(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    for (uint16_t j = 0; j < h; j++) {
        for (uint16_t i = 0; i < w; i++) {
            const uint8_t L = (uint8_t)((uint32_t)i * 255 / (w - 1));
            gui_test_pixel(fb, (uint16_t)(x + i), (uint16_t)(y + j), L, L, L);
        }
    }
}

/**
 * @brief 渲染后端自检（异步架构）：每步 flush 后 wait_idle 确认画面已更新，
 *        worker 日志给出各次刷新的真实耗时。
 */
static void gui_selftest_task(void *arg) {
    (void)arg;
    espaperplay_gui_framebuffer_t fb;
    esp_err_t ret;

    ESP_LOGI(TAG, "GUI backend selftest started (RGB565 + async worker)");
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 1. 清白。 */
    ret = espaperplay_gui_clear();
    ESP_LOGI(TAG, "selftest: clear queued -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    espaperplay_gui_wait_idle(5000);
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* 2. 纯黑白 UI（BAYER 完整性）：白底黑框 + 中央黑方块。 */
    ret = espaperplay_gui_get_framebuffer(&fb);
    if (ret != ESP_OK) {
        goto out;
    }
    gui_test_fill(fb.buffer, 8, 8, 784, 16, 0, 0, 0);
    gui_test_fill(fb.buffer, 8, 456, 784, 16, 0, 0, 0);
    gui_test_fill(fb.buffer, 8, 8, 16, 464, 0, 0, 0);
    gui_test_fill(fb.buffer, 776, 8, 16, 464, 0, 0, 0);
    gui_test_fill(fb.buffer, 360, 190, 80, 80, 0, 0, 0);
    espaperplay_gui_submit_area(0, 0, fb.width, fb.height);
    ret = espaperplay_gui_flush();
    ESP_LOGI(TAG, "selftest: bw pure-black UI (bayer) queued -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    espaperplay_gui_wait_idle(5000);
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* 3. 灰度渐变（BAYER 局部刷新）。 */
    gui_test_gradient(fb.buffer, 200, 60, 200, 120);
    espaperplay_gui_submit_area(200, 60, 200, 120);
    ret = espaperplay_gui_flush();
    ESP_LOGI(TAG, "selftest: gradient bayer queued -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    espaperplay_gui_wait_idle(5000);
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* 4. 阈值对照。 */
    gui_test_gradient(fb.buffer, 440, 60, 160, 120);
    ret = espaperplay_gui_set_converter(ESPAPERPLAY_GUI_CONVERTER_THRESHOLD);
    if (ret != ESP_OK) {
        goto out;
    }
    espaperplay_gui_submit_area(440, 60, 160, 120);
    ret = espaperplay_gui_flush();
    ESP_LOGI(TAG, "selftest: gradient threshold queued -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    espaperplay_gui_wait_idle(5000);
    vTaskDelay(pdMS_TO_TICKS(1000));
    espaperplay_gui_set_converter(ESPAPERPLAY_GUI_CONVERTER_BAYER);

    /* 5. 转换性能（直接调用转换器）。 */
    {
        int64_t t0;
        t0 = esp_timer_get_time();
        gui_convert_bw(fb.buffer, 0, 0, s_disp_w, s_disp_h,
                       s_stage_a_bw);
        ESP_LOGI(TAG, "perf: bw bayer full convert -> %lld ms", (esp_timer_get_time() - t0) / 1000);
        s_converter = ESPAPERPLAY_GUI_CONVERTER_THRESHOLD;
        t0 = esp_timer_get_time();
        gui_convert_bw(fb.buffer, 0, 0, s_disp_w, s_disp_h,
                       s_stage_a_bw);
        ESP_LOGI(TAG, "perf: bw threshold full convert -> %lld ms",
                 (esp_timer_get_time() - t0) / 1000);
        s_converter = ESPAPERPLAY_GUI_CONVERTER_BAYER;
        s_gray4_dither = ESPAPERPLAY_GUI_GRAY4_DITHER_BAYER;
        t0 = esp_timer_get_time();
        gui_convert_gray4(s_fb_rgb, s_stage_a_gray4);
        ESP_LOGI(TAG, "perf: gray4 bayer full convert -> %lld ms",
                 (esp_timer_get_time() - t0) / 1000);
        s_gray4_dither = ESPAPERPLAY_GUI_GRAY4_DITHER_FS;
        t0 = esp_timer_get_time();
        gui_convert_gray4(s_fb_rgb, s_stage_a_gray4);
        ESP_LOGI(TAG, "perf: gray4 floyd-steinberg full convert -> %lld ms",
                 (esp_timer_get_time() - t0) / 1000);
    }

    /* 6. gray4 高清（FS，默认）。 */
    gui_test_fill(fb.buffer, 0, 0, 200, 480, 0, 0, 0);
    gui_test_fill(fb.buffer, 200, 0, 200, 480, 85, 85, 85);
    gui_test_gradient(fb.buffer, 400, 0, 200, 480);
    gui_test_fill(fb.buffer, 600, 0, 200, 480, 255, 255, 255);
    ret = espaperplay_gui_show_gray4();
    ESP_LOGI(TAG, "selftest: gray4 full (FS) queued -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    espaperplay_gui_wait_idle(8000);
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* 7. gray4 高清（Bayer）对比。 */
    ret = espaperplay_gui_set_gray4_dither(ESPAPERPLAY_GUI_GRAY4_DITHER_BAYER);
    if (ret != ESP_OK) {
        goto out;
    }
    ret = espaperplay_gui_show_gray4();
    ESP_LOGI(TAG, "selftest: gray4 full (Bayer) queued -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    espaperplay_gui_wait_idle(8000);
    vTaskDelay(pdMS_TO_TICKS(1000));
    espaperplay_gui_set_gray4_dither(ESPAPERPLAY_GUI_GRAY4_DITHER_FS);

    /* 8. 模式切换残留：主帧置全白后 show_bw，面板应纯白。 */
    memset(fb.buffer, 0xFF, s_fb_rgb_bytes);
    ret = espaperplay_gui_show_bw();
    ESP_LOGI(TAG, "selftest: show_bw after gray4 queued -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    espaperplay_gui_wait_idle(5000);
    vTaskDelay(pdMS_TO_TICKS(1000));

out:
    ret = espaperplay_gui_clear();
    ESP_LOGI(TAG, "GUI selftest done, clear queued -> %s", esp_err_to_name(ret));
    espaperplay_gui_wait_idle(5000);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
#endif /* ESPAPERPLAY_GUI_ENABLE_SELFTEST */

/* ====================================================================
 * 公共 API
 * ==================================================================== */

static uint8_t *gui_alloc(size_t bytes) {
    uint8_t *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return p;
}

esp_err_t espaperplay_gui_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }
    /* 分辨率来自运行时显示参数（espaperplay_display），支持不同面板/横竖屏。 */
    s_disp_w = espaperplay_display_width();
    s_disp_h = espaperplay_display_height();
    s_fb_pixels = (uint32_t)s_disp_w * s_disp_h;
    s_fb_rgb_bytes = (size_t)s_fb_pixels * 2;
    s_fb_bw_bytes = s_fb_pixels / 8;
    s_fb_gray4_bytes = s_fb_pixels / 4;
    s_stride_rgb = (size_t)s_disp_w * 2;
    s_full_threshold = (uint32_t)s_fb_pixels * ESPAPERPLAY_GUI_FULL_AREA_RATIO / 100;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_fs_err = (int16_t *)gui_alloc((size_t)(s_disp_w + 2) * 2 * sizeof(int16_t));
    s_fb_rgb = gui_alloc(s_fb_rgb_bytes);
    s_stage_a_bw = gui_alloc(s_fb_bw_bytes);
    s_stage_b_bw = gui_alloc(s_fb_bw_bytes);
    s_stage_a_gray4 = gui_alloc(s_fb_gray4_bytes);
    s_stage_b_gray4 = gui_alloc(s_fb_gray4_bytes);
    if (s_fs_err == NULL || s_fb_rgb == NULL || s_stage_a_bw == NULL || s_stage_b_bw == NULL ||
        s_stage_a_gray4 == NULL || s_stage_b_gray4 == NULL) {
        ESP_LOGE(TAG, "alloc failed (rgb=%u bw=%u+%u gray4=%u+%u)", (unsigned)s_fb_rgb_bytes,
                 (unsigned)s_fb_bw_bytes, (unsigned)s_fb_bw_bytes, (unsigned)s_fb_gray4_bytes,
                 (unsigned)s_fb_gray4_bytes);
        heap_caps_free(s_fs_err);
        heap_caps_free(s_fb_rgb);
        heap_caps_free(s_stage_a_bw);
        heap_caps_free(s_stage_b_bw);
        heap_caps_free(s_stage_a_gray4);
        heap_caps_free(s_stage_b_gray4);
        s_fs_err = NULL;
        s_fb_rgb = s_stage_a_bw = s_stage_b_bw = s_stage_a_gray4 = s_stage_b_gray4 = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb_rgb, 0xFF, s_fb_rgb_bytes);
    s_color = ESPAPERPLAY_GUI_COLOR_BW;
    /* 默认阈值转换：LVGL 抗锯齿字体在字形边缘产生中间灰，Bayer 抖动会把
     * 其点阵化（边缘发糊）；阈值直接切边，文字锐利。照片/图像走 GRAY4
     * 模式（独立抖动），不受影响；需要 Bayer 时应用可 set_converter 切换。 */
    s_converter = ESPAPERPLAY_GUI_CONVERTER_THRESHOLD;
    s_gray4_dither = ESPAPERPLAY_GUI_GRAY4_DITHER_FS;
    s_dirty = false;
    s_op_a.state = GUI_SLOT_IDLE;
    s_op_b.state = GUI_SLOT_IDLE;
    s_initialized = true;

    /* 异步刷新 worker（创建失败退化为同步执行，仅告警）。 */
    if (xTaskCreate(gui_worker_task, "gui_epd_worker", 4096, NULL, 6, &s_worker_task) != pdPASS) {
        ESP_LOGW(TAG, "worker task create failed: refreshes run synchronously");
        s_worker_task = NULL;
    }

    ESP_LOGI(TAG, "GUI backend ready: rgb %uB + 2x bw %uB + 2x gray4 %uB (%s)",
             (unsigned)s_fb_rgb_bytes, (unsigned)s_fb_bw_bytes, (unsigned)s_fb_gray4_bytes,
             s_worker_task != NULL ? "async worker" : "sync fallback");

#if ESPAPERPLAY_GUI_ENABLE_SELFTEST
    xTaskCreate(gui_selftest_task, "gui_selftest", 4096, NULL, 5, NULL);
#endif /* ESPAPERPLAY_GUI_ENABLE_SELFTEST */

    return ESP_OK;
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
        s_dirty = false;
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

esp_err_t espaperplay_gui_set_full_force_after(uint32_t count) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count > ESPAPERPLAY_GUI_FULL_FORCE_AFTER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (count != s_full_force_after) {
        s_full_force_after = count;
        s_large_partial_count = 0; /* 阈值变化，重新开始计数 */
        ESP_LOGI(TAG, "full force after: %u (%s)", (unsigned)count, count ? "enabled" : "disabled");
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

uint32_t espaperplay_gui_get_full_force_after(void) { return s_full_force_after; }

esp_err_t espaperplay_gui_get_framebuffer(espaperplay_gui_framebuffer_t *fb) {
    if (fb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    fb->buffer = s_fb_rgb;
    fb->width = s_disp_w;
    fb->height = s_disp_h;
    fb->stride = s_stride_rgb;
    fb->color = s_color;
    return ESP_OK;
}

esp_err_t espaperplay_gui_submit_area(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    uint16_t xa, x_end, wa;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width == 0 || height == 0) {
        return ESP_OK;
    }
    if (x >= s_disp_w || y >= s_disp_h) {
        return ESP_OK;
    }
    if ((uint32_t)x + width > s_disp_w) {
        width = s_disp_w - x;
    }
    if ((uint32_t)y + height > s_disp_h) {
        height = s_disp_h - y;
    }
    xa = x & (uint16_t)~7u;
    x_end = (uint16_t)((x + width + 7) & ~7u);
    if (x_end > s_disp_w) {
        x_end = s_disp_w;
    }
    wa = x_end - xa;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_dirty) {
        s_dirty_x = xa;
        s_dirty_y = y;
        s_dirty_w = wa;
        s_dirty_h = height;
        s_dirty = true;
    } else {
        const uint16_t x0 = s_dirty_x < xa ? s_dirty_x : xa;
        const uint16_t y0 = s_dirty_y < y ? s_dirty_y : y;
        const uint16_t x1 =
            (uint16_t)(s_dirty_x + s_dirty_w) > x_end ? (uint16_t)(s_dirty_x + s_dirty_w) : x_end;
        const uint16_t y1 = (uint16_t)(s_dirty_y + s_dirty_h) > (uint16_t)(y + height)
                                ? (uint16_t)(s_dirty_y + s_dirty_h)
                                : (uint16_t)(y + height);
        s_dirty_x = x0;
        s_dirty_y = y0;
        s_dirty_w = (uint16_t)(x1 - x0);
        s_dirty_h = (uint16_t)(y1 - y0);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/**
 * @brief 把当前脏区并入一个已就绪槽的窗口（包围盒），供重快照取最新帧。
 *
 * 仅用于 READY 且 type==FRAME 的槽（CLEAR 槽无有效窗口，不允许合并）。
 */
static void gui_merge_dirty(const gui_op_t *op) {
    const uint16_t x0 = op->x < s_dirty_x ? op->x : s_dirty_x;
    const uint16_t y0 = op->y < s_dirty_y ? op->y : s_dirty_y;
    const uint16_t x1 = (uint16_t)(op->x + op->w) > (uint16_t)(s_dirty_x + s_dirty_w)
                            ? (uint16_t)(op->x + op->w)
                            : (uint16_t)(s_dirty_x + s_dirty_w);
    const uint16_t y1 = (uint16_t)(op->y + op->h) > (uint16_t)(s_dirty_y + s_dirty_h)
                            ? (uint16_t)(op->y + op->h)
                            : (uint16_t)(s_dirty_y + s_dirty_h);
    s_dirty_x = x0;
    s_dirty_y = y0;
    s_dirty_w = (uint16_t)(x1 - x0);
    s_dirty_h = (uint16_t)(y1 - y0);
}

esp_err_t espaperplay_gui_flush(void) {
    esp_err_t ret = ESP_OK;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_dirty) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    /* 选槽：优先空闲槽（快照）；其次 READY 的 FRAME 槽（并入重快照取最新帧）。
     * BUSY 槽正被 worker 的 SPI 传输读取，严禁触碰——否则会撕裂在途帧。 */
    gui_op_t *target = NULL;
    if (s_op_a.state == GUI_SLOT_IDLE) {
        target = &s_op_a;
        gui_snapshot(target, s_stage_a_bw, s_stage_a_gray4);
    } else if (s_op_b.state == GUI_SLOT_IDLE) {
        target = &s_op_b;
        gui_snapshot(target, s_stage_b_bw, s_stage_b_gray4);
    } else if (s_op_b.state == GUI_SLOT_READY && s_op_b.type == GUI_OP_FRAME) {
        target = &s_op_b;
        gui_merge_dirty(target);
        gui_snapshot(target, s_stage_b_bw, s_stage_b_gray4);
    } else if (s_op_a.state == GUI_SLOT_READY && s_op_a.type == GUI_OP_FRAME) {
        target = &s_op_a;
        gui_merge_dirty(target);
        gui_snapshot(target, s_stage_a_bw, s_stage_a_gray4);
    } else {
        /* 两个槽都无法写入（双双 BUSY 或唯一 READY 槽是 CLEAR）：理论不可达
         * 或极短暂窗口。丢弃本帧但保留脏区，下一帧自动重试。 */
        ESP_LOGE(TAG, "no writable refresh slot, frame dropped (retry next flush)");
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_worker_task != NULL) {
        xTaskNotifyGive(s_worker_task);
    } else {
        /* 退化同步路径：无并发，直接执行并复位槽位。 */
        gui_count_executed(target);
        ret = gui_execute_op(target);
        target->state = GUI_SLOT_IDLE;
    }

    s_dirty = false;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t espaperplay_gui_wait_idle(uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    for (;;) {
        bool busy;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            busy = s_op_a.state != GUI_SLOT_IDLE || s_op_b.state != GUI_SLOT_IDLE;
            xSemaphoreGive(s_lock);
        } else {
            busy = true;
        }
        if (!busy) {
            return ESP_OK;
        }
        if (xTaskGetTickCount() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t espaperplay_gui_show_bw(void) {
    esp_err_t ret = espaperplay_gui_set_color(ESPAPERPLAY_GUI_COLOR_BW);
    if (ret != ESP_OK) {
        return ret;
    }
    espaperplay_gui_submit_area(0, 0, s_disp_w, s_disp_h);
    return espaperplay_gui_flush();
}

esp_err_t espaperplay_gui_show_gray4(void) {
    esp_err_t ret = espaperplay_gui_set_color(ESPAPERPLAY_GUI_COLOR_GRAY4);
    if (ret != ESP_OK) {
        return ret;
    }
    espaperplay_gui_submit_area(0, 0, s_disp_w, s_disp_h);
    return espaperplay_gui_flush();
}

esp_err_t espaperplay_gui_full_refresh(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* 选一个非 BUSY 槽承载全刷快照（BUSY 槽正被 worker 的 SPI 传输读取，
     * 必须保留）；另一非 BUSY 槽的排队帧一并丢弃——全刷建立新基线，
     * 旧帧无需再刷（与 gui_clear 的做法一致）。 */
    gui_op_t *slot = NULL;
    if (s_op_a.state != GUI_SLOT_BUSY) {
        slot = &s_op_a;
    } else if (s_op_b.state != GUI_SLOT_BUSY) {
        slot = &s_op_b;
    } else {
        /* 两槽皆 BUSY：单 worker 下不可达；防御性拒绝，避免覆盖在途帧。 */
        ESP_LOGE(TAG, "full refresh: no free slot (both busy)");
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* 整帧快照（不改变当前模式）：BW -> 1bpp 全帧 + 强制全像素翻转全刷
     * （清残影）；GRAY4 -> 整帧四灰阶全屏刷新。 */
    slot->type = GUI_OP_FRAME;
    slot->color = s_color;
    slot->x = 0;
    slot->y = 0;
    slot->w = s_disp_w;
    slot->h = s_disp_h;
    slot->large_area = false;
    if (s_color == ESPAPERPLAY_GUI_COLOR_GRAY4) {
        uint8_t *stage = (slot == &s_op_a) ? s_stage_a_gray4 : s_stage_b_gray4;
        gui_convert_gray4(s_fb_rgb, stage);
        slot->stage = stage;
        slot->force_full = false;
    } else {
        uint8_t *stage = (slot == &s_op_a) ? s_stage_a_bw : s_stage_b_bw;
        gui_convert_bw(s_fb_rgb, 0, 0, s_disp_w, s_disp_h, stage);
        slot->stage = stage;
        slot->force_full = true; /* FULL_FORCE：全像素深波形，清残影 */
    }
    slot->state = GUI_SLOT_READY;

    if (slot == &s_op_a) {
        if (s_op_b.state != GUI_SLOT_BUSY) {
            s_op_b.state = GUI_SLOT_IDLE;
        }
    } else {
        if (s_op_a.state != GUI_SLOT_BUSY) {
            s_op_a.state = GUI_SLOT_IDLE;
        }
    }
    s_dirty = false;
    s_large_partial_count = 0; /* 全刷建立新基线，大面积局刷计数归零 */

    if (s_worker_task != NULL) {
        xTaskNotifyGive(s_worker_task);
    } else {
        const esp_err_t ret = gui_execute_op(slot);
        slot->state = GUI_SLOT_IDLE;
        xSemaphoreGive(s_lock);
        return ret;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t espaperplay_gui_clear(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_fb_rgb, 0xFF, s_fb_rgb_bytes);
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* 选一个非 BUSY 槽承载清白操作（IDLE 或 READY 皆可复用）；BUSY 槽正被
     * worker 的 SPI 传输读取，必须保留。另一非 BUSY 槽的排队帧一并丢弃。 */
    gui_op_t *slot = NULL;
    if (s_op_a.state != GUI_SLOT_BUSY) {
        slot = &s_op_a;
    } else if (s_op_b.state != GUI_SLOT_BUSY) {
        slot = &s_op_b;
    } else {
        /* 两槽皆 BUSY：单 worker 下不可达；防御性拒绝，避免覆盖在途帧。 */
        ESP_LOGE(TAG, "clear: no free slot (both busy)");
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    slot->state = GUI_SLOT_READY;
    slot->type = GUI_OP_CLEAR;
    slot->color = s_color;
    slot->stage = NULL;
    slot->large_area = false;
    slot->force_full = false;
    slot->x = slot->y = slot->w = slot->h = 0;

    /* 丢弃另一槽的排队帧（BUSY 槽保留）。 */
    if (slot == &s_op_a) {
        if (s_op_b.state != GUI_SLOT_BUSY) {
            s_op_b.state = GUI_SLOT_IDLE;
        }
    } else {
        if (s_op_a.state != GUI_SLOT_BUSY) {
            s_op_a.state = GUI_SLOT_IDLE;
        }
    }
    s_dirty = false;

    if (s_worker_task != NULL) {
        xTaskNotifyGive(s_worker_task);
    } else {
        const esp_err_t ret = gui_execute_op(slot);
        slot->state = GUI_SLOT_IDLE;
        xSemaphoreGive(s_lock);
        return ret;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
