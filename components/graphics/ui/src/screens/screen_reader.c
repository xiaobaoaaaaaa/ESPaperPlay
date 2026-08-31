/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps */
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "espaperplay_fonts.h"
#include "espaperplay_gui.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_input.h"
#include "espaperplay_reader.h"
#include "espaperplay_reader_history.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"

#include "lvgl.h"
#include "lvgl_private.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 阅读器页（阅读视图）
 * ====================================================================
 *
 * 布局：统一状态栏（"阅读器"）+ 正文区 + 底部页码。交互：
 *   - 点击左侧 1/3 或左滑 -> 上一页；点击右侧 1/3 或右滑 -> 下一页；
 *   - 点击中部 1/3 -> 展开底边栏（上一页 / 页码跳转 / 下一页 / 字号 - +
 *     / 单次 GRAY4 / 返回）；
 *   - 屏幕左右边缘向内滑动 -> 返回上一页；
 *   - BOOT 键：单击下一页、双击上一页、长按单次 GRAY4 刷屏。
 *
 * 性能（大文件）：惰性分页——打开时只计算第一页，翻页时按需计算当前页
 * 边界；后台任务继续计算剩余页边界以得到总页数（页码显示 n / m）。512KB
 * 小说打开即时、翻页不卡。
 *
 * 页窗口（预载 + 释放）：显示某页时预载「前后各 READER_WINDOW 页」的文本
 * 缓冲，翻页只换标签不重建缓冲；窗口外的页文本被释放。任意页访问（跳转）
 * 通过稀疏检查点索引随机定位（每 READER_SPARSE_STEP 页一个检查点），无需
 * 从头线性计算分页边界。
 *
 * 阅读进度：退出页面时把当前页写入 SD 卡历史（reader 组件 worker 异步落
 * 盘），再次打开同一本书自动恢复进度。
 */

#define READER_LINE_SPACE 4
#define READER_MARGIN 16
#define READER_STATUS_H 30
#define READER_FOOTER_H 30
#define READER_EDGE_PX 24
#define READER_EDGE_SWIPE_PX 70
#define READER_SWIPE_PX 90
#define READER_CLICK_MAX_PX 15
#define READER_SWIPE_MIN_RATIO 1.2f

/* 字号档位（固定集合，避免挤爆 FreeType 缓存） */
static const int READER_FONT_SIZES[] = {16, 20, 24, 32};
#define READER_FONT_CNT ((int)(sizeof(READER_FONT_SIZES) / sizeof(READER_FONT_SIZES[0])))
#define READER_FONT_DEFAULT_IDX 1 /* 20px */

#define READER_PAGES_CAP_INIT 256 /* 分页表初始容量（页数） */

/* 后台总页数任务：栈须放内部 RAM（读 PSRAM 文本 + flash 字体，且不能占 PSRAM
 * 栈；lv_text_get_next_line -> FreeType 字形查找栈深较大，3072 曾触发溢出）。 */
#define READER_TOTAL_TASK_STACK 8192
#define READER_TOTAL_TASK_PRIO 3

/* 稀疏检查点索引：每 READER_SPARSE_STEP 页记录一次（页号, 字节偏移），
 * 使任意页访问/跳转只需从最近检查点线性推进 ≤STEP 页，而不是从头重算。 */
#define READER_SPARSE_STEP 25
#define READER_SPARSE_MAX 160 /* 覆盖 160*25=4000 页（512KB 中文约 1300 页） */

/* 页窗口：显示当前页时预载前后各 READER_WINDOW 页的文本缓冲 */
#define READER_WINDOW 1
#define READER_WIN_CNT (2 * READER_WINDOW + 1)

#define READER_FONT_NAME                                                                           \
    (espaperplay_system_get_config()                                                               \
         ->selected_font) /* 当前选用字体（SD 优先，缺则回退 Flash 子集） */

/* ------------------------------------------------------------------ */
/* 页面状态                                                             */
/* ------------------------------------------------------------------ */

static espaperplay_ui_status_bar_t *s_bar = NULL; /*!< 统一状态栏 */
static lv_obj_t *s_content_label = NULL;          /*!< 正文标签 */
static lv_obj_t *s_page_label = NULL;             /*!< 底部页码 */
static lv_obj_t *s_hint_label = NULL;             /*!< 提示（无文档/空文件） */

static const char *s_text = NULL; /*!< 归一化文本（reader 组件持有） */
static size_t s_text_len = 0;     /*!< 文本长度 */
static lv_font_t *s_font = NULL;  /*!< 当前字体 */
static int s_font_idx = READER_FONT_DEFAULT_IDX;
static int s_cur_page = 0; /*!< 当前页（0 基） */
static bool s_ready = false;
static int s_content_w = 0;
static int s_content_h = 0;
static bool s_gray4_display = false; /*!< 单次 GRAY4 显示模式（期间暂停状态栏刷新） */

/* 惰性分页表（互斥保护：LVGL 翻页 + 后台总页数任务并发访问） */
static struct {
    uint32_t
        *offsets;  /*!< offsets[i] = 第 i 页起始字节偏移；offsets[computed] = 已计算的最后边界 */
    int cap;       /*!< 容量（offsets 有效项 cap+1） */
    int computed;  /*!< 已计算的页边界数（offsets[0..computed] 有效） */
    int total;     /*!< 已知总页数（-1=未知；offsets[computed]>=文本长度时可知） */
    int max_lines; /*!< 每页最大行数（依赖字号） */
} s_pages;
static SemaphoreHandle_t s_pages_lock = NULL;
static TaskHandle_t s_total_task = NULL;
static volatile bool s_total_stop = false;
static lv_text_attributes_t s_attr; /*!< 换行属性（BREAK_ALL + 内容区宽度） */

/* 稀疏检查点索引（页号, 字节偏移；按页号升序，n≤READER_SPARSE_MAX 线性查找即可） */
typedef struct {
    int page;     /*!< 页号（0 基） */
    uint32_t off; /*!< 该页起始字节偏移 */
} reader_sparse_t;
static reader_sparse_t s_sparse[READER_SPARSE_MAX];
static int s_sparse_cnt = 0;

/* 页窗口缓存：当前页 ± READER_WINDOW 页的文本缓冲（PSRAM）；窗口外页被释放 */
typedef struct {
    int idx;    /*!< 页号（-1=空槽） */
    char *text; /*!< NUL 结尾页文本（PSRAM，持有） */
} reader_pagebuf_t;
static reader_pagebuf_t s_pcache[READER_WIN_CNT];

/* 底边栏 / 跳转输入 */
static lv_obj_t *s_bar_overlay = NULL;    /*!< 底边栏覆盖层（NULL=未展开） */
static lv_obj_t *s_bar_panel = NULL;      /*!< 底边栏面板 */
static lv_obj_t *s_bar_page_label = NULL; /*!< 底边栏页码按钮文本 */
static lv_obj_t *s_jump_modal = NULL;     /*!< 跳转输入模态 */
static lv_obj_t *s_jump_ta = NULL;        /*!< 跳转输入框 */
static lv_timer_t *s_gray4_timer = NULL;  /*!< 延迟灰度一次性定时器（底边栏按钮触发） */

/* 手势 */
static bool s_touch_down = false;
static lv_point_t s_touch_start = {0, 0};
static lv_point_t s_touch_last = {0, 0};

/* 前向声明 */
static void reader_show_page(int idx);
static void reader_footer_update(void);
static void reader_bar_close(void);
static void reader_jump_open(void);
static void reader_gray4_deferred_cb(lv_timer_t *t);
static void reader_gray4_mode_set(bool on);

/* ------------------------------------------------------------------ */
/* 工具                                                                 */
/* ------------------------------------------------------------------ */

/** FreeType 字体按需加载（当前字号档位）。 */
static lv_font_t *reader_font(void) {
    const char *name = READER_FONT_NAME[0] ? READER_FONT_NAME : ESPAPERPLAY_FONTS_DEFAULT_NAME;
    lv_font_t *f = espaperplay_fonts_load(name, (uint32_t)READER_FONT_SIZES[s_font_idx],
                                          ESPAPERPLAY_FONT_STYLE_NORMAL);
    if (f == NULL) {
        f = espaperplay_fonts_load(ESPAPERPLAY_FONTS_DEFAULT_NAME,
                                   (uint32_t)READER_FONT_SIZES[s_font_idx],
                                   ESPAPERPLAY_FONT_STYLE_NORMAL);
    }
    return f;
}

/** 逻辑分辨率（旋转后）。 */
static void reader_screen_size(int32_t *out_w, int32_t *out_h) {
    lv_display_t *disp = lv_display_get_default();
    *out_w = lv_display_get_horizontal_resolution(disp);
    *out_h = lv_display_get_vertical_resolution(disp);
}

/* ------------------------------------------------------------------ */
/* 惰性分页                                                             */
/* ------------------------------------------------------------------ */

/** 稀疏检查点：按页号升序插入/更新（n≤READER_SPARSE_MAX，线性即可）。 */
static void rdr_sparse_add(int page, uint32_t off) {
    for (int i = 0; i < s_sparse_cnt; i++) {
        if (s_sparse[i].page == page) {
            s_sparse[i].off = off;
            return;
        }
        if (s_sparse[i].page > page) {
            if (s_sparse_cnt >= READER_SPARSE_MAX) {
                return;
            }
            for (int j = s_sparse_cnt; j > i; j--) {
                s_sparse[j] = s_sparse[j - 1];
            }
            s_sparse[i].page = page;
            s_sparse[i].off = off;
            s_sparse_cnt++;
            return;
        }
    }
    if (s_sparse_cnt < READER_SPARSE_MAX) {
        s_sparse[s_sparse_cnt].page = page;
        s_sparse[s_sparse_cnt].off = off;
        s_sparse_cnt++;
    }
}

/** 查找页号 ≤ page 的最近检查点下标（无则 -1）。 */
static int rdr_sparse_find(int page) {
    int best = -1;
    for (int i = 0; i < s_sparse_cnt; i++) {
        if (s_sparse[i].page <= page) {
            best = i;
        } else {
            break;
        }
    }
    return best;
}

/** 页窗口缓存：清空全部槽（释放文本缓冲）。 */
static void reader_pcache_reset(void) {
    for (int i = 0; i < READER_WIN_CNT; i++) {
        if (s_pcache[i].text != NULL) {
            free(s_pcache[i].text);
            s_pcache[i].text = NULL;
        }
        s_pcache[i].idx = -1;
    }
}

/** 释放分页表（须持锁）。 */
static void reader_pages_free_locked(void) {
    if (s_pages.offsets != NULL) {
        heap_caps_free(s_pages.offsets);
        s_pages.offsets = NULL;
    }
    s_pages.cap = 0;
    s_pages.computed = 0;
    s_pages.total = -1;
    s_pages.max_lines = 0;
}

/** 初始化分页表（须持锁；s_font/s_content_w/s_content_h 须已就绪）。 */
static bool reader_pages_init_locked(void) {
    s_pages.cap = READER_PAGES_CAP_INIT;
    s_pages.offsets = heap_caps_malloc((size_t)(s_pages.cap + 1) * sizeof(uint32_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pages.offsets == NULL) {
        s_pages.offsets =
            heap_caps_malloc((size_t)(s_pages.cap + 1) * sizeof(uint32_t), MALLOC_CAP_8BIT);
    }
    if (s_pages.offsets == NULL) {
        s_pages.cap = 0;
        return false;
    }
    s_pages.offsets[0] = 0;
    s_pages.computed = 0;
    s_pages.total = -1;
    s_sparse_cnt = 0; /* 分页参数变化（字号等）：稀疏索引失效重建 */
    const int lh = lv_font_get_line_height(s_font) + READER_LINE_SPACE;
    s_pages.max_lines = (lh > 0 && s_content_h > 0) ? (s_content_h / lh) : 1;
    if (s_pages.max_lines < 1) {
        s_pages.max_lines = 1;
    }
    lv_text_attributes_init(&s_attr);
    s_attr.letter_space = 0;
    s_attr.line_space = READER_LINE_SPACE;
    s_attr.max_width = s_content_w;
    s_attr.text_flags = LV_TEXT_FLAG_BREAK_ALL;
    return true;
}

/**
 * 按需计算分页边界，确保 offsets[want_end_idx] 已就绪（须持锁）。
 * 计算到文本末尾时设置 s_pages.total。幂等，可被 LVGL 与后台任务并发调用。
 */
static void reader_compute_pages_locked(int want_end_idx) {
    /* 远跳（目标远超已计算边界）：从最近稀疏检查点回退，避免从头线性重算 */
    if (want_end_idx > s_pages.computed && want_end_idx - s_pages.computed > READER_SPARSE_STEP &&
        s_sparse_cnt > 0) {
        const int best = rdr_sparse_find(want_end_idx);
        if (best >= 0) {
            s_pages.computed = s_sparse[best].page;
            s_pages.offsets[s_pages.computed] = s_sparse[best].off;
        }
    }
    while (s_pages.computed < want_end_idx && s_pages.offsets[s_pages.computed] < s_text_len) {
        uint32_t pos = s_pages.offsets[s_pages.computed];
        int lines = 0;
        bool stuck = false;
        while (pos < s_text_len && lines < s_pages.max_lines) {
            const uint32_t adv = lv_text_get_next_line(&s_text[pos], (uint32_t)(s_text_len - pos),
                                                       s_font, NULL, &s_attr);
            if (adv == 0) {
                stuck = true; /* 无法前进（如字体缺失）：停止分页，避免死循环 */
                break;
            }
            pos += adv;
            lines++;
        }
        if (s_pages.computed + 1 >= s_pages.cap) {
            const int nc = s_pages.cap * 2;
            uint32_t *nb = heap_caps_realloc(s_pages.offsets, (size_t)(nc + 1) * sizeof(uint32_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (nb == NULL) {
                nb = heap_caps_realloc(s_pages.offsets, (size_t)(nc + 1) * sizeof(uint32_t),
                                       MALLOC_CAP_8BIT);
            }
            if (nb == NULL) {
                ESP_LOGE(TAG, "pages realloc failed (at %d)", s_pages.computed);
                break;
            }
            s_pages.offsets = nb;
            s_pages.cap = nc;
        }
        s_pages.computed++;
        s_pages.offsets[s_pages.computed] = pos;
        /* 记录稀疏检查点（幂等，供远跳回退） */
        if (s_pages.computed % READER_SPARSE_STEP == 0) {
            rdr_sparse_add(s_pages.computed, pos);
        }
        if (stuck) {
            break;
        }
    }
    if (s_pages.offsets != NULL && s_pages.offsets[s_pages.computed] >= s_text_len) {
        s_pages.total = s_pages.computed;
    }
}

/** 后台任务完成回调：刷新页码显示（LVGL 线程）。 */
static void reader_total_done_lv(void *arg) {
    (void)arg;
    if (s_page_label != NULL) {
        reader_footer_update();
    }
}

/** 后台任务：继续计算剩余页边界以得到总页数。 */
static void reader_total_task(void *arg) {
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_pages_lock, portMAX_DELAY) != pdTRUE) {
            break;
        }
        if (s_total_stop || s_pages.offsets == NULL || s_text == NULL) {
            xSemaphoreGive(s_pages_lock);
            break;
        }
        if (s_pages.total >= 0) {
            xSemaphoreGive(s_pages_lock);
            break;
        }
        reader_compute_pages_locked(s_pages.computed + 1); /* 每次只推进一个边界 */
        const bool done = (s_pages.total >= 0);
        xSemaphoreGive(s_pages_lock);
        if (done) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    s_total_task = NULL;
    (void)espaperplay_gui_lv_call(reader_total_done_lv, NULL, 1000);
    vTaskDelete(NULL);
}

/** 启动后台总页数计算（幂等）。 */
static void reader_total_task_start(void) {
    if (s_total_task != NULL || s_pages.offsets == NULL || s_text == NULL) {
        return;
    }
    s_total_stop = false;
    /* 栈放内部 RAM：任务读 PSRAM 文本 + flash 字体，且 CONFIG_SPIRAM_USE_MALLOC
     * 下 xTaskCreate 默认栈在 PSRAM（flash 缓存禁用窗口无法访问，且栈深大） */
    if (xTaskCreateWithCaps(reader_total_task, "rdr_total", READER_TOTAL_TASK_STACK, NULL,
                            READER_TOTAL_TASK_PRIO, &s_total_task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_total_task = NULL;
        ESP_LOGE(TAG, "total page task create failed");
    }
}

/* ------------------------------------------------------------------ */
/* 翻页 / 页码                                                          */
/* ------------------------------------------------------------------ */

/** 刷新页码显示（底部 + 底边栏）。 */
static void reader_footer_update(void) {
    if (s_page_label == NULL) {
        return;
    }
    char buf[32];
    if (s_pages.total >= 0) {
        snprintf(buf, sizeof(buf), "%d / %d", s_cur_page + 1, s_pages.total);
    } else {
        snprintf(buf, sizeof(buf), "%d / …", s_cur_page + 1);
    }
    lv_label_set_text(s_page_label, buf);
    if (s_bar_page_label != NULL) {
        lv_label_set_text(s_bar_page_label, buf);
    }
}

/**
 * 显示指定页（0 基；按需计算边界，越界自动钳制）。
 *
 * 页窗口策略：进入目标页后，一次性确保「前后各 READER_WINDOW 页」的边界
 * 与文本缓冲就绪（预载），并释放窗口外的页文本（占用仅 READER_WIN_CNT 份
 * 缓冲）。翻页时新邻居在上一轮已预载，标签直接换源，快速响应。
 */
static void reader_show_page(int idx) {
    if (!s_ready || s_content_label == NULL || s_pages.offsets == NULL) {
        return;
    }

    /* 显示新页即退出灰度显示模式（翻页/字号变化的 BW 刷新将执行全屏基线清残留） */
    reader_gray4_mode_set(false);

    /* 窗口页范围与字节区间（须持锁，避免与后台任务并发） */
    int lo = 0;
    int hi = 0;
    uint32_t rng_s[READER_WIN_CNT];
    uint32_t rng_e[READER_WIN_CNT];

    if (xSemaphoreTake(s_pages_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    /* 目标页 + 窗口内最远页的边界一起计算 */
    reader_compute_pages_locked(idx + 1 + READER_WINDOW);
    if (s_pages.total >= 0 && idx >= s_pages.total) {
        idx = s_pages.total - 1;
    }
    if (idx < 0) {
        idx = 0;
    }
    if (s_pages.total == 0) {
        xSemaphoreGive(s_pages_lock);
        lv_label_set_text(s_content_label, "");
        s_cur_page = 0;
        reader_footer_update();
        return;
    }
    lo = idx - READER_WINDOW;
    if (lo < 0) {
        lo = 0;
    }
    hi = idx + READER_WINDOW;
    if (s_pages.total >= 0 && hi >= s_pages.total) {
        hi = s_pages.total - 1;
    }
    /* 总页数未知时的兜底：确保窗口最远页边界已计算 */
    if (s_pages.computed < hi + 1) {
        reader_compute_pages_locked(hi + 1);
    }
    const int cnt = hi - lo + 1;
    for (int i = 0; i < cnt; i++) {
        rng_s[i] = s_pages.offsets[lo + i];
        rng_e[i] = s_pages.offsets[lo + i + 1];
    }
    xSemaphoreGive(s_pages_lock);

    s_cur_page = idx;

    /* 释放窗口外的页文本（不再相邻） */
    for (int i = 0; i < READER_WIN_CNT; i++) {
        if (s_pcache[i].text != NULL && (s_pcache[i].idx < lo || s_pcache[i].idx > hi)) {
            free(s_pcache[i].text);
            s_pcache[i].text = NULL;
            s_pcache[i].idx = -1;
        }
    }
    /* 补足窗口内缺失的页文本（预载前后页，供快速翻页） */
    for (int p = lo; p <= hi; p++) {
        bool found = false;
        for (int i = 0; i < READER_WIN_CNT; i++) {
            if (s_pcache[i].idx == p && s_pcache[i].text != NULL) {
                found = true;
                break;
            }
        }
        if (found) {
            continue;
        }
        const int k = p - lo;
        const uint32_t len = rng_e[k] - rng_s[k];
        char *buf = heap_caps_malloc((size_t)len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            buf = malloc((size_t)len + 1);
        }
        if (buf == NULL) {
            continue;
        }
        memcpy(buf, &s_text[rng_s[k]], len);
        buf[len] = '\0';
        bool placed = false;
        for (int i = 0; i < READER_WIN_CNT; i++) {
            if (s_pcache[i].text == NULL) {
                s_pcache[i].idx = p;
                s_pcache[i].text = buf;
                placed = true;
                break;
            }
        }
        if (!placed) {
            free(buf); /* 防御：无空槽（正常驱逐后不可能） */
        }
    }

    /* 用缓存中的当前页文本设置标签（无需重建） */
    const char *txt = NULL;
    for (int i = 0; i < READER_WIN_CNT; i++) {
        if (s_pcache[i].idx == idx && s_pcache[i].text != NULL) {
            txt = s_pcache[i].text;
            break;
        }
    }
    lv_label_set_text(s_content_label, txt != NULL ? txt : "");
    reader_footer_update();
    ESP_LOGI(TAG, "reader: page %d (window %d..%d)", s_cur_page + 1, lo + 1, hi + 1);
}

static void reader_next_page(void) {
    if (!s_ready) {
        return;
    }
    if (s_pages.total >= 0 && s_cur_page + 1 >= s_pages.total) {
        ESP_LOGI(TAG, "reader: already last page");
        return;
    }
    reader_show_page(s_cur_page + 1);
}

static void reader_prev_page(void) {
    if (!s_ready) {
        return;
    }
    if (s_cur_page <= 0) {
        ESP_LOGI(TAG, "reader: already first page");
        return;
    }
    reader_show_page(s_cur_page - 1);
}

/* ------------------------------------------------------------------ */
/* 字号 / 灰阶                                                          */
/* ------------------------------------------------------------------ */

/** 重新初始化分页（字号变化后），保留当前页索引（越界自动钳制）。 */
static void reader_reinit_pages(void) {
    reader_pcache_reset(); /* 旧字号页文本失效 */
    xSemaphoreTake(s_pages_lock, portMAX_DELAY);
    reader_pages_free_locked();
    if (s_font != NULL && s_text != NULL && s_text_len > 0) {
        (void)reader_pages_init_locked();
        reader_compute_pages_locked(1);
    }
    xSemaphoreGive(s_pages_lock);
    if (s_ready) {
        reader_show_page(s_cur_page);
        reader_total_task_start();
    }
}

/** 设置字号档位。 */
static void reader_set_font(int idx) {
    if (idx < 0 || idx >= READER_FONT_CNT || idx == s_font_idx) {
        return;
    }
    s_font_idx = idx;
    s_font = reader_font();
    if (s_content_label != NULL && s_font != NULL) {
        lv_obj_set_style_text_font(s_content_label, s_font, 0);
    }
    if (s_ready) {
        reader_reinit_pages();
    }
}

/**
 * 设置/退出「灰度显示」模式。
 *
 * 进入：单次 GRAY4 后，状态栏的周期刷新必须暂停——灰阶后的下一次 BW 刷新
 * 会被后端升级为全屏基线（清灰阶残留，见 espaperplay_gui 的 s_pending_bw_clean），
 * 状态栏内容一变化（分钟跳变 / WiFi 图标）就会触发整屏 BW 覆写、灰度失效。
 * 退出（翻页 / 字号变化 / 打开底边栏 / 页面退出）：恢复状态栏刷新，此时任何
 * BW 刷新（通常是整屏内容变化）才执行全屏基线清残留。
 */
static void reader_gray4_mode_set(bool on) {
    if (s_gray4_display == on) {
        return;
    }
    s_gray4_display = on;
    espaperplay_ui_status_bar_set_suspended(on);
    ESP_LOGI(TAG, "reader: gray4 display %s (status bar %s)", on ? "on" : "off",
             on ? "suspended" : "resumed");
}

/**
 * 单次 GRAY4 刷屏：当前帧按四灰阶全屏刷新一次，随后恢复 BW 模式。
 * 恢复后下一次 BW 刷新由渲染后端自动升级为全屏基线（清除灰阶残留，
 * 见 espaperplay_gui 的 s_pending_bw_clean）。
 */
static void reader_do_gray4_once(void) {
    ESP_LOGI(TAG, "reader: single GRAY4 refresh");
    (void)espaperplay_gui_show_gray4();
    (void)espaperplay_gui_set_color(ESPAPERPLAY_GUI_COLOR_BW);
    reader_gray4_mode_set(true); /* 保持灰阶画面：暂停状态栏刷新 */
}

/* ------------------------------------------------------------------ */
/* 底边栏                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    READER_BAR_PREV = 0,
    READER_BAR_NEXT,
    READER_BAR_JUMP,
    READER_BAR_FONT_DOWN,
    READER_BAR_FONT_UP,
    READER_BAR_GRAY4,
    READER_BAR_BACK,
} reader_bar_action_t;

/** 底边栏按钮回调（按 user_data 分发动作）。 */
static void reader_bar_action_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    const intptr_t a = (intptr_t)lv_event_get_user_data(e);
    switch (a) {
    case READER_BAR_PREV:
        reader_prev_page();
        break;
    case READER_BAR_NEXT:
        reader_next_page();
        break;
    case READER_BAR_JUMP:
        reader_jump_open();
        break;
    case READER_BAR_FONT_DOWN:
        reader_set_font(s_font_idx - 1);
        break;
    case READER_BAR_FONT_UP:
        reader_set_font(s_font_idx + 1);
        break;
    case READER_BAR_GRAY4:
        /* 先关底边栏让正文重绘；灰阶快照推迟到下一帧——否则会拍到含底边栏的
         * 旧帧，且帧末 BW 局刷会立刻覆盖灰阶（灰度瞬间失效）。 */
        reader_bar_close();
        if (s_gray4_timer == NULL) {
            s_gray4_timer = lv_timer_create(reader_gray4_deferred_cb, 50, NULL);
            lv_timer_set_repeat_count(s_gray4_timer, 1);
        }
        break;
    case READER_BAR_BACK:
        reader_bar_close();
        if (espaperplay_ui_page_depth() > 1) {
            espaperplay_ui_page_pop_lv();
        }
        break;
    default:
        break;
    }
}

/** 延迟灰度回调：底边栏关闭、正文重绘完成后对正文执行单次 GRAY4（LVGL 线程）。 */
static void reader_gray4_deferred_cb(lv_timer_t *t) {
    lv_timer_delete(t);
    s_gray4_timer = NULL;
    if (s_ready) {
        reader_do_gray4_once();
    }
}

/** 底边栏内通用按钮（白底黑边），返回按钮内的标签对象（供页码刷新）。 */
static lv_obj_t *reader_bar_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                                   intptr_t action) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(lbl, s_font, 0);
    }
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, reader_bar_action_cb, LV_EVENT_CLICKED, (void *)action);
    return lbl;
}

/** 底边栏覆盖层点击：关闭。 */
static void reader_bar_overlay_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    reader_bar_close();
}

/** 关闭底边栏。 */
static void reader_bar_close(void) {
    if (s_bar_overlay != NULL) {
        lv_obj_del(s_bar_overlay);
        s_bar_overlay = NULL;
        s_bar_panel = NULL;
        s_bar_page_label = NULL;
    }
}

/** 展开底边栏（两行：翻页/跳转 + 字号/灰度/返回）。 */
static void reader_bar_open(void) {
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    reader_screen_size(&scr_w, &scr_h);

    /* 打开底边栏会整屏重绘，退出灰度显示模式（恢复状态栏刷新） */
    reader_gray4_mode_set(false);

    /* 全屏覆盖层：拦截触摸，点空白关闭。背景透明——底边栏只是工具栏，
     * 正文需保持可见；浅灰背景在 BW 模式下会渲染成纯白把正文整片盖掉。 */
    s_bar_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_bar_overlay, scr_w, scr_h);
    lv_obj_set_pos(s_bar_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_bar_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bar_overlay, 0, 0);
    lv_obj_set_style_radius(s_bar_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_bar_overlay, 0, 0);
    lv_obj_remove_flag(s_bar_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_bar_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_bar_overlay, reader_bar_overlay_cb, LV_EVENT_CLICKED, NULL);

    const int margin = READER_MARGIN;
    const int pw = scr_w - 2 * margin;
    const int bh = 40;
    const int gap = 8;
    const int pad = 10;
    const int ph = pad + bh + gap + bh + pad;
    const int py = scr_h - ph - 6;

    s_bar_panel = lv_obj_create(s_bar_overlay);
    lv_obj_set_size(s_bar_panel, pw, ph);
    lv_obj_set_pos(s_bar_panel, margin, py);
    lv_obj_set_style_bg_color(s_bar_panel, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_bar_panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_bar_panel, 2, 0);
    lv_obj_set_style_radius(s_bar_panel, 12, 0);
    lv_obj_set_style_pad_all(s_bar_panel, 0, 0);
    lv_obj_remove_flag(s_bar_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 行 1：上一页 / 页码（点击跳转）/ 下一页 */
    int y = pad;
    const int w1 = (pw - 2 * pad - 2 * gap) / 3;
    reader_bar_button(s_bar_panel, "◀", pad, y, w1, bh, READER_BAR_PREV);
    s_bar_page_label =
        reader_bar_button(s_bar_panel, "", pad + w1 + gap, y, w1, bh, READER_BAR_JUMP);
    reader_bar_button(s_bar_panel, "▶", pad + 2 * (w1 + gap), y, w1, bh, READER_BAR_NEXT);
    y += bh + gap;

    /* 行 2：A- / A+ / 灰度 / 返回 */
    const int w2 = (pw - 2 * pad - 3 * gap) / 4;
    int x = pad;
    reader_bar_button(s_bar_panel, "A-", x, y, w2, bh, READER_BAR_FONT_DOWN);
    x += w2 + gap;
    reader_bar_button(s_bar_panel, "A+", x, y, w2, bh, READER_BAR_FONT_UP);
    x += w2 + gap;
    reader_bar_button(s_bar_panel, "灰度", x, y, w2, bh, READER_BAR_GRAY4);
    x += w2 + gap;
    reader_bar_button(s_bar_panel, "返回", x, y, w2, bh, READER_BAR_BACK);

    reader_footer_update(); /* 页码同步到底边栏 */
    ESP_LOGI(TAG, "reader: bottom bar open");
}

/** 切换底边栏。 */
static void reader_bar_toggle(void) {
    if (s_bar_overlay != NULL) {
        reader_bar_close();
    } else {
        reader_bar_open();
    }
}

/* ------------------------------------------------------------------ */
/* 跳转输入模态（LVGL 数字键盘）                                         */
/* ------------------------------------------------------------------ */

static void reader_jump_close(void) {
    if (s_jump_modal != NULL) {
        lv_obj_del(s_jump_modal);
        s_jump_modal = NULL;
    }
    s_jump_ta = NULL;
}

/** 跳转 确定：解析页码并跳转（未知总数时按需计算，越界钳制）。 */
static void reader_jump_ok_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_jump_ta == NULL) {
        return;
    }
    const char *txt = lv_textarea_get_text(s_jump_ta);
    long p = strtol(txt, NULL, 10);
    if (p < 1) {
        p = 1;
    }
    if (p > 99999) {
        p = 99999; /* 防御：避免病态输入触发超长按需计算 */
    }
    reader_jump_close();
    reader_bar_close();
    reader_show_page((int)p - 1);
}

/** 跳转 取消。 */
static void reader_jump_cancel_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    reader_jump_close();
}

/** 打开跳转输入模态（底部面板 + 数字键盘）。 */
static void reader_jump_open(void) {
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    reader_screen_size(&scr_w, &scr_h);

    const int margin = READER_MARGIN;
    const int pad = 10;
    const int title_h = 30;
    const int ta_h = 40;
    const int status_h = 22;
    const int bh = 38;
    const int kb_h = 170;
    const int panel_w = scr_w - 2 * margin;
    const int panel_h = pad + title_h + 6 + ta_h + 4 + status_h + 6 + bh + pad;
    const int kb_y = scr_h - kb_h - 6;
    const int panel_y = kb_y - panel_h - 6;

    s_jump_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_jump_modal, scr_w, scr_h);
    lv_obj_set_pos(s_jump_modal, 0, 0);
    lv_obj_set_style_bg_color(s_jump_modal, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_border_width(s_jump_modal, 0, 0);
    lv_obj_set_style_radius(s_jump_modal, 0, 0);
    lv_obj_set_style_pad_all(s_jump_modal, 0, 0);
    lv_obj_remove_flag(s_jump_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_jump_modal, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *panel = lv_obj_create(s_jump_modal);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_pos(panel, margin, panel_y);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "跳转到页");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(title, s_font, 0);
    }
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_pos(title, 0, pad);

    s_jump_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_jump_ta, panel_w - 2 * pad, ta_h);
    lv_obj_set_pos(s_jump_ta, pad, pad + title_h + 6);
    lv_textarea_set_one_line(s_jump_ta, true);
    lv_textarea_set_max_length(s_jump_ta, 6);
    lv_textarea_set_accepted_chars(s_jump_ta, "0123456789");
    lv_textarea_set_placeholder_text(s_jump_ta, "页码");
    lv_obj_set_style_text_color(s_jump_ta, lv_color_black(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(s_jump_ta, s_font, 0);
    }
    lv_obj_set_style_border_color(s_jump_ta, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_jump_ta, 2, 0);
    lv_obj_set_style_radius(s_jump_ta, 6, 0);
    lv_obj_set_style_pad_left(s_jump_ta, 8, 0);
    /* 墨水屏：禁用光标闪烁（避免连续局部刷新） */
    lv_obj_set_style_anim_duration(s_jump_ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(s_jump_ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    lv_obj_t *status = lv_label_create(panel);
    char hint[48];
    if (s_pages.total >= 0) {
        snprintf(hint, sizeof(hint), "请输入 1 - %d", s_pages.total);
    } else {
        snprintf(hint, sizeof(hint), "请输入页码");
    }
    lv_label_set_text(status, hint);
    lv_obj_set_style_text_color(status, lv_color_black(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(status, s_font, 0);
    }
    lv_obj_set_width(status, LV_PCT(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(status, pad + 2, pad + title_h + 6 + ta_h + 4);

    const int bw = (panel_w - 2 * pad - 12) / 2;
    const int btn_y = pad + title_h + 6 + ta_h + 4 + status_h + 6;

    /* 取消：关闭跳转 */
    lv_obj_t *cancel_btn = lv_button_create(panel);
    lv_obj_set_size(cancel_btn, bw, bh);
    lv_obj_set_pos(cancel_btn, pad, btn_y);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_white(), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(cancel_btn, 2, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_color(cl, lv_color_black(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(cl, s_font, 0);
    }
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel_btn, reader_jump_cancel_cb, LV_EVENT_CLICKED, NULL);

    /* 确定：跳转 */
    lv_obj_t *ok_btn = lv_button_create(panel);
    lv_obj_set_size(ok_btn, bw, bh);
    lv_obj_set_pos(ok_btn, pad + bw + 12, btn_y);
    lv_obj_set_style_bg_color(ok_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_t *ol = lv_label_create(ok_btn);
    lv_label_set_text(ol, "确定");
    lv_obj_set_style_text_color(ol, lv_color_white(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(ol, s_font, 0);
    }
    lv_obj_center(ol);
    lv_obj_add_event_cb(ok_btn, reader_jump_ok_cb, LV_EVENT_CLICKED, NULL);

    /* 数字键盘（挂在模态上贴底） */
    lv_obj_t *kb = lv_keyboard_create(s_jump_modal);
    lv_obj_set_size(kb, panel_w, kb_h);
    lv_obj_align(kb, LV_ALIGN_TOP_LEFT, margin, kb_y);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, s_jump_ta);

    ESP_LOGI(TAG, "reader: jump input open");
}

/* ------------------------------------------------------------------ */
/* 页面生命周期                                                          */
/* ------------------------------------------------------------------ */

/** 阅读视图构建（页面 enter：屏幕已由页面栈清空）。 */
static void reader_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t scr_w = 0;
    int32_t scr_h = 0;
    reader_screen_size(&scr_w, &scr_h);

    s_bar = espaperplay_ui_status_bar_create(scr, READER_STATUS_H, "阅读器", false);
    espaperplay_ui_status_bar_refresh(s_bar);

    s_content_w = scr_w - 2 * READER_MARGIN;
    s_content_h = scr_h - READER_STATUS_H - READER_FOOTER_H - 2 * READER_MARGIN;
    if (s_content_h < 100) {
        s_content_h = 100;
    }
    const int content_y = READER_STATUS_H + READER_MARGIN;

    /* 阅读视图默认 BW 交互模式 */
    (void)espaperplay_gui_set_color(ESPAPERPLAY_GUI_COLOR_BW);

    /* 正文标签 */
    s_content_label = lv_label_create(scr);
    lv_obj_set_pos(s_content_label, READER_MARGIN, content_y);
    lv_obj_set_size(s_content_label, s_content_w, s_content_h);
    lv_obj_set_style_text_color(s_content_label, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_content_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_content_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_all(s_content_label, 0, 0);
    lv_obj_remove_flag(s_content_label, LV_OBJ_FLAG_SCROLLABLE);
    s_font = reader_font();
    if (s_font != NULL) {
        lv_obj_set_style_text_font(s_content_label, s_font, 0);
    }

    /* 底部页码 */
    s_page_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_page_label, lv_color_black(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(s_page_label, s_font, 0);
    }
    lv_obj_set_style_text_align(s_page_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_page_label, LV_PCT(100));
    lv_obj_set_pos(s_page_label, 0, scr_h - READER_FOOTER_H + 6);
    lv_label_set_text(s_page_label, "");

    /* 提示标签（无文档/空文件） */
    s_hint_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_hint_label, lv_color_black(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(s_hint_label, s_font, 0);
    }
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_hint_label, LV_PCT(100));
    lv_obj_set_pos(s_hint_label, 0, scr_h / 2 - 20);
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);

    /* 文本来源 */
    s_text = NULL;
    s_text_len = 0;
    if (espaperplay_reader_is_open()) {
        (void)espaperplay_reader_get_text(&s_text, &s_text_len);
    }
    s_ready = false;
    s_cur_page = 0;
    s_bar_overlay = NULL;
    s_bar_panel = NULL;
    s_bar_page_label = NULL;
    s_jump_modal = NULL;
    s_jump_ta = NULL;
    s_gray4_timer = NULL;         /* 退出已取消；防御性复位 */
    reader_gray4_mode_set(false); /* 防御：确保状态栏刷新恢复、灰度标志复位 */
    reader_pcache_reset();        /* 旧文档/旧字号的窗口页缓冲失效 */

    if (s_pages_lock == NULL) {
        s_pages_lock = xSemaphoreCreateMutex();
    }

    xSemaphoreTake(s_pages_lock, portMAX_DELAY);
    reader_pages_free_locked();
    const bool ok = reader_pages_init_locked();
    reader_compute_pages_locked(1);
    xSemaphoreGive(s_pages_lock);
    if (!ok) {
        lv_label_set_text(s_hint_label, "内存不足");
        lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_content_label, "");
        lv_label_set_text(s_page_label, "0 / 0");
        s_ready = true;
        ESP_LOGE(TAG, "reader: pages alloc failed");
        return;
    }
    s_ready = true;

    if (s_text == NULL || s_text_len == 0) {
        const char *hint = espaperplay_reader_is_open() ? "空文件" : "无文档";
        lv_label_set_text(s_hint_label, hint);
        lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_content_label, "");
        lv_label_set_text(s_page_label, "0 / 0");
        ESP_LOGI(TAG, "reader: no document");
    } else {
        lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        /* 从历史恢复进度 */
        uint32_t start = 0;
        const char *path = espaperplay_reader_get_path();
        if (path != NULL) {
            espaperplay_reader_history_entry_t e;
            if (espaperplay_reader_history_find(path, &e) == ESP_OK) {
                start = e.page;
            }
        }
        reader_show_page((int)start);
        reader_total_task_start();
    }

    s_touch_down = false;
    ESP_LOGI(TAG, "reader view entered (%u bytes)", (unsigned)s_text_len);
}

/** 阅读视图退出：保存进度、停后台任务、释放资源。 */
static void reader_exit(void) {
    /* 保存阅读进度（异步投递历史写） */
    if (s_ready && s_cur_page >= 0 && espaperplay_reader_is_open()) {
        const char *path = espaperplay_reader_get_path();
        if (path != NULL) {
            const int total = (s_pages.total >= 0) ? s_pages.total : 0;
            (void)espaperplay_reader_history_update(path, (uint32_t)s_cur_page, (uint32_t)total);
            ESP_LOGI(TAG, "reader: save progress page %d", s_cur_page + 1);
        }
    }
    /* 停后台总页数任务（任务每次迭代都检查停止标志，很快退出） */
    if (s_total_task != NULL) {
        s_total_stop = true;
        for (int i = 0; i < 60 && s_total_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    s_total_stop = false;
    reader_gray4_mode_set(false); /* 退出灰度显示模式：恢复状态栏刷新 */
    reader_pcache_reset();        /* 释放窗口页文本缓冲 */
    if (s_pages_lock != NULL) {
        xSemaphoreTake(s_pages_lock, portMAX_DELAY);
        reader_pages_free_locked();
        xSemaphoreGive(s_pages_lock);
    }
    if (s_bar_overlay != NULL) {
        lv_obj_del(s_bar_overlay);
        s_bar_overlay = NULL;
        s_bar_panel = NULL;
        s_bar_page_label = NULL;
    }
    if (s_jump_modal != NULL) {
        lv_obj_del(s_jump_modal);
        s_jump_modal = NULL;
        s_jump_ta = NULL;
    }
    if (s_gray4_timer != NULL) {
        lv_timer_delete(s_gray4_timer);
        s_gray4_timer = NULL;
    }
    s_bar = NULL;
    s_content_label = NULL;
    s_page_label = NULL;
    s_hint_label = NULL;
    s_text = NULL;
    s_text_len = 0;
    s_ready = false;
    s_touch_down = false;
    ESP_LOGI(TAG, "reader view exited");
}

/* ------------------------------------------------------------------ */
/* 按键 / 触摸                                                          */
/* ------------------------------------------------------------------ */

/** 按键处理：单击下一页（有覆盖层时先关闭）、双击上一页、长按单次 GRAY4。 */
static void reader_on_key(const espaperplay_input_event_t *event) {
    if (event->type != ESPAPERPLAY_INPUT_EVENT_KEY) {
        return;
    }
    switch (event->key_action) {
    case ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK:
        if (s_jump_modal != NULL) {
            reader_jump_close();
        } else if (s_bar_overlay != NULL) {
            reader_bar_close();
        } else {
            reader_next_page();
        }
        break;
    case ESPAPERPLAY_INPUT_KEY_ACTION_DOUBLE_CLICK:
        if (s_bar_overlay == NULL && s_jump_modal == NULL) {
            reader_prev_page();
        }
        break;
    case ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_START:
        if (s_bar_overlay == NULL && s_jump_modal == NULL) {
            reader_do_gray4_once();
        }
        break;
    default:
        break;
    }
}

/** 触摸处理：边缘滑动返回；左滑上一页 / 右滑下一页；点击左/中/右 1/3 分区。 */
static void reader_on_touch(const espaperplay_input_event_t *event) {
    lv_point_t p;
    espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

    /* 底边栏 / 跳转模态打开：点击由覆盖层与按钮处理，页面手势让位 */
    if (s_bar_overlay != NULL || s_jump_modal != NULL) {
        return;
    }

    if (event->touch_pressed) {
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start = p;
        }
        s_touch_last = p;
        return;
    }
    if (!s_touch_down) {
        return;
    }
    s_touch_down = false;
    const int dx = s_touch_last.x - s_touch_start.x;
    const int dy = s_touch_last.y - s_touch_start.y;
    const int adx = abs(dx);
    const int ady = abs(dy);

    /* 边缘向内滑动：返回上一页 */
    if (adx > READER_EDGE_SWIPE_PX && adx > ady * READER_SWIPE_MIN_RATIO) {
        int32_t scr_w = 0;
        int32_t scr_h = 0;
        reader_screen_size(&scr_w, &scr_h);
        if ((s_touch_start.x < READER_EDGE_PX && dx > 0) ||
            (s_touch_start.x > scr_w - READER_EDGE_PX && dx < 0)) {
            if (espaperplay_ui_page_depth() > 1) {
                ESP_LOGI(TAG, "reader: edge swipe -> back");
                espaperplay_ui_page_pop_lv();
            }
            return;
        }
    }

    /* 横滑：左滑下一页 / 右滑上一页（实机确认原方向相反，已调换） */
    if (adx > READER_SWIPE_PX && adx > ady * READER_SWIPE_MIN_RATIO) {
        if (dx < 0) {
            reader_next_page();
        } else {
            reader_prev_page();
        }
        return;
    }

    /* 点击分区：左 1/3 上一页，右 1/3 下一页，中 1/3 展开底边栏。
     *
     * 坑：释放帧事件不带坐标（input_touch_event_cb 投递释放时 point 全 0），
     * 本帧映射后的 p 恒为固定角点——竖屏旋转下 map(0,0) 落在右 1/3，会把
     * 所有点击都判成"下一页"（快速滑动按下+释放落在两次采样之间、dx≈0 时
     * 也会落入本分支）。分区判定必须用按下起点 s_touch_start（真实坐标，
     * 与文件/设置页按下时命中检测同款）。 */
    if (adx <= READER_CLICK_MAX_PX && ady <= READER_CLICK_MAX_PX) {
        int32_t scr_w = 0;
        int32_t scr_h = 0;
        reader_screen_size(&scr_w, &scr_h);
        if (s_touch_start.x < scr_w / 3) {
            reader_prev_page();
        } else if (s_touch_start.x >= scr_w * 2 / 3) {
            reader_next_page();
        } else {
            reader_bar_toggle();
        }
    }
}

/** 阅读视图页面实例。 */
const espaperplay_ui_page_t espaperplay_ui_page_reader_view = {reader_enter, reader_exit,
                                                               reader_on_key, reader_on_touch};
