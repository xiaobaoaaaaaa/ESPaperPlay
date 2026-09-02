/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * 阅读器页（阅读视图，TXT / EPUB 通用）
 * ====================================================================
 *
 * 面向统一块模型渲染（见 espaperplay_reader_blocks.h）：
 *   - 分页单位是「章节内页」：从页起点（块号, 行号）出发，按各块的字体
 *     （标题层级 / 粗斜体）逐块逐行填充内容区，得到页片段表（文本段字节
 *     区间 / 图片段）并返回下一页起点；图片块独占一页（居中显示）。
 *   - 全局页码 = 前缀章节页数和 + 章内页号。各章页数表惰性计算：恢复进度
 *     时同步算到目标章，其余由 LVGL 定时器分片补齐（每 tick 一章）；
 *   - TXT 为单章节文档（每行为一块），沿用惰性分页 + 稀疏检查点，512KB
 *     小说打开即时；EPUB 章节小，按需加载驻留（同一时刻仅一章在内存）。
 *
 * 交互不变：点击左/中/右 1/3（上页 / 底边栏 / 下页）、横滑翻页、边缘内滑
 * 返回；BOOT 键单击下页、双击上页、长按单次 GRAY4；底边栏（翻页 / 页码
 * 跳转 / 字号 / 灰度 / 返回）对两种格式复用。
 */

#define READER_LINE_SPACE 4
#define READER_PAR_SPACE 4    /* 段后额外间距 */
#define READER_QUOTE_INDENT 24
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

#define READER_PSTART_CAP_INIT 256  /* 章内页起点表初始容量 */
#define READER_SEG_MAX 64           /* 单页片段数上限 */
#define READER_SPARSE_STEP 25       /* 稀疏检查点步长（页） */
#define READER_SPARSE_MAX 160
#define READER_CH_PAGES_UNSET 0xFFFFFFFFu

#define READER_FONT_NAME                                                                           \
    (espaperplay_system_get_config()                                                               \
         ->selected_font) /* 当前选用字体（SD 优先，缺则回退 Flash 子集） */

/* ------------------------------------------------------------------ */
/* 页面状态                                                             */
/* ------------------------------------------------------------------ */

static espaperplay_ui_status_bar_t *s_bar = NULL;  /*!< 统一状态栏 */
static lv_obj_t *s_content = NULL;                 /*!< 正文容器（页片段挂载点） */
static lv_obj_t *s_page_label = NULL;              /*!< 底部页码 */
static lv_obj_t *s_hint_label = NULL;              /*!< 提示（无文档/空文件） */
static lv_font_t *s_font = NULL;                   /*!< 正文字体 */
static int s_font_idx = READER_FONT_DEFAULT_IDX;
static bool s_ready = false;
static int s_content_w = 0;
static int s_content_h = 0;
static int s_content_y = 0;
static bool s_gray4_display = false; /*!< 单次 GRAY4 显示模式（期间暂停状态栏刷新） */

/* 文档状态 */
static int s_ch = 0;         /*!< 当前章节（0 基） */
static int s_local_page = 0; /*!< 章内当前页（0 基） */
static uint32_t *s_ch_pages = NULL; /*!< 各章页数（READER_CH_PAGES_UNSET=未知，PSRAM） */
static int s_chapter_cnt = 0;
static bool s_ch_zero_pstart = false; /*!< 驻留章为空（无页） */

/* 驻留章惰性分页表（LVGL 线程内串行访问） */
typedef struct {
    uint32_t block; /*!< 页起始块号 */
    uint16_t line;  /*!< 页起始块内行号 */
} reader_pos_t;
static reader_pos_t *s_pstarts = NULL; /*!< pstarts[i] = 第 i 页起点 */
static int s_pstart_ch = -1;          /*!< pstarts / 章总页数记录归属的章节（=当前驻留章） */
static int s_pstart_cap = 0;
static int s_pstart_cnt = 0;  /*!< 已计算的页起点数（页 0..cnt-1） */
static bool s_ch_total_known = false;

typedef struct {
    const espaperplay_reader_block_t *blk; /*!< 所属块（NULL=图片段用 flags） */
    uint32_t off;    /*!< 文本段：章节 blob 内起始偏移 */
    uint32_t len;    /*!< 文本段：字节长度 */
    uint16_t flags;  /*!< 块样式 */
    int32_t image;   /*!< >=0 图片段 */
    int y;           /*!< 页内 y 偏移 */
    int x;           /*!< 页内 x 偏移 */
    int w;           /*!< 段宽 */
} reader_seg_t;
static reader_seg_t s_segs[READER_SEG_MAX];

/* 稀疏检查点（章内远跳回退；page → pos） */
typedef struct {
    int page;
    reader_pos_t pos;
} reader_sparse_t;
static reader_sparse_t s_sparse[READER_SPARSE_MAX];
static int s_sparse_cnt = 0;

static lv_timer_t *s_total_timer = NULL;  /*!< 总页数分片计算定时器 */
static lv_timer_t *s_loading_timer = NULL; /*!< 延迟初始化定时器 */
static lv_obj_t *s_loading_modal = NULL;
static uint32_t s_pending_start = 0;      /*!< 待恢复的全局起始页 */
static bool s_loading = false;
static uint32_t s_last_footer_tick = 0;
static char s_footer_last[48];  /*!< 上次页码文本（去重，避免墨水屏无谓局刷） */

/* 底边栏 / 跳转输入 */
static lv_obj_t *s_bar_overlay = NULL;
static lv_obj_t *s_bar_panel = NULL;
static lv_obj_t *s_bar_page_label = NULL;
static lv_obj_t *s_jump_modal = NULL;
static lv_obj_t *s_jump_ta = NULL;
static lv_timer_t *s_gray4_timer = NULL;

/* 手势 */
static bool s_touch_down = false;
static lv_point_t s_touch_start = {0, 0};
static lv_point_t s_touch_last = {0, 0};

/* 前向声明 */
static void reader_show_page(int ch, int local);
static void reader_toc_close(void);
static void reader_toc_open(void);
static void reader_toc_row_cb(lv_event_t *e);
static void reader_footer_update(void);
static void reader_bar_close(void);
static void reader_jump_open(void);
static void reader_gray4_deferred_cb(lv_timer_t *t);
static void reader_gray4_mode_set(bool on);

/* ------------------------------------------------------------------ */
/* 工具                                                                 */
/* ------------------------------------------------------------------ */

/** 逻辑分辨率（旋转后）。 */
static void reader_screen_size(int32_t *out_w, int32_t *out_h) {
    lv_display_t *disp = lv_display_get_default();
    *out_w = lv_display_get_horizontal_resolution(disp);
    *out_h = lv_display_get_vertical_resolution(disp);
}

/** 基础字号（当前档位）。 */
static int reader_base_size(void) { return READER_FONT_SIZES[s_font_idx]; }

/** FreeType 字体按需加载（带样式；espaperplay_fonts_load 内部有 (名,号,样式) 缓存）。 */
static lv_font_t *reader_font_styled(int size_px, int style) {
    const char *name = READER_FONT_NAME[0] ? READER_FONT_NAME : ESPAPERPLAY_FONTS_DEFAULT_NAME;
    lv_font_t *f = espaperplay_fonts_load(name, (uint32_t)size_px,
                                          (espaperplay_font_style_t)style);
    if (f == NULL) {
        f = espaperplay_fonts_load(ESPAPERPLAY_FONTS_DEFAULT_NAME, (uint32_t)size_px,
                                   (espaperplay_font_style_t)style);
    }
    return f;
}

/** 块字体：标题按层级放大（粗体），粗 / 斜体通过 FreeType 样式合成。 */
static lv_font_t *reader_block_font(uint16_t flags) {
    int style = ESPAPERPLAY_FONT_STYLE_NORMAL;
    if ((flags & ESPAPERPLAY_READER_BLK_BOLD) != 0) {
        style |= ESPAPERPLAY_FONT_STYLE_BOLD;
    }
    if ((flags & ESPAPERPLAY_READER_BLK_ITALIC) != 0) {
        style |= ESPAPERPLAY_FONT_STYLE_ITALIC;
    }
    int size = reader_base_size();
    switch (ESPAPERPLAY_READER_BLK_HEAD_LEVEL(flags)) {
    case 1:
        size = reader_base_size() + 12;
        style |= ESPAPERPLAY_FONT_STYLE_BOLD;
        break;
    case 2:
        size = reader_base_size() + 8;
        style |= ESPAPERPLAY_FONT_STYLE_BOLD;
        break;
    case 3:
        size = reader_base_size() + 4;
        style |= ESPAPERPLAY_FONT_STYLE_BOLD;
        break;
    case 4:
    case 5:
    case 6:
        style |= ESPAPERPLAY_FONT_STYLE_BOLD;
        break;
    default:
        break;
    }
    if (size > 40) {
        size = 40;
    }
    return reader_font_styled(size, style);
}

/* ------------------------------------------------------------------ */
/* 章内惰性分页                                                         */
/* ------------------------------------------------------------------ */

/** 稀疏检查点：按页号升序插入/更新。 */
static void rdr_sparse_add(int page, reader_pos_t pos) {
    for (int i = 0; i < s_sparse_cnt; i++) {
        if (s_sparse[i].page == page) {
            s_sparse[i].pos = pos;
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
            s_sparse[i].pos = pos;
            s_sparse_cnt++;
            return;
        }
    }
    if (s_sparse_cnt < READER_SPARSE_MAX) {
        s_sparse[s_sparse_cnt].page = page;
        s_sparse[s_sparse_cnt].pos = pos;
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

static void reader_pstarts_free(void) {
    if (s_pstarts != NULL) {
        heap_caps_free(s_pstarts);
        s_pstarts = NULL;
    }
    s_pstart_cap = 0;
    s_pstart_cnt = 0;
    s_ch_total_known = false;
    s_sparse_cnt = 0;
}

static uint32_t s_paginate_t0 = 0; /*!< 当前章分页计时起点（进度日志用） */

/** 版式指纹：字号档位 + 内容区宽高（变化使分页缓存失效）。 */
static uint32_t reader_layout_key(void) {
    return ((uint32_t)s_font_idx & 0x3u) | ((uint32_t)s_content_w << 2) |
           ((uint32_t)s_content_h << 12);
}

/**
 * 重置指定章的分页表（章加载后调用）。
 *
 * @param ch 分页表归属章节；必须在此处（查缓存之前）设定 s_pstart_ch——
 *           归属滞后会把别的章的缓存页边界/页数套到本章（历史缺陷）。
 */
static bool reader_pstarts_init(int ch) {
    s_pstart_ch = ch;
    reader_pstarts_free();
    s_pstart_cap = READER_PSTART_CAP_INIT;
    s_pstarts = heap_caps_malloc((size_t)s_pstart_cap * sizeof(reader_pos_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pstarts == NULL) {
        s_pstarts =
            heap_caps_malloc((size_t)s_pstart_cap * sizeof(reader_pos_t), MALLOC_CAP_8BIT);
    }
    if (s_pstarts == NULL) {
        s_pstart_cap = 0;
        return false;
    }
    s_pstarts[0].block = 0;
    s_pstarts[0].line = 0;
    s_pstart_cnt = 1;
    s_paginate_t0 = lv_tick_get();

    /* 分页缓存：命中则整章页边界即刻就绪（省去 FreeType 逐行测量；TXT/EPUB 通用） */
    {
        uint32_t *bt = heap_caps_malloc((size_t)s_pstart_cap * sizeof(uint32_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint16_t *lt = heap_caps_malloc((size_t)s_pstart_cap * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (bt != NULL && lt != NULL) {
            int cnt = espaperplay_reader_pagen_load(ch, reader_layout_key(), bt, lt,
                                                    s_pstart_cap);
            if (cnt < 0) {
                /* 大章节：缓存有效但表容量不足 → 扩表后重读一次 */
                const int need = -cnt;
                reader_pos_t *grown =
                    heap_caps_realloc(s_pstarts, (size_t)need * sizeof(reader_pos_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (grown == NULL) {
                    grown = heap_caps_realloc(s_pstarts, (size_t)need * sizeof(reader_pos_t),
                                              MALLOC_CAP_8BIT);
                }
                if (grown != NULL) {
                    s_pstarts = grown;
                    s_pstart_cap = need;
                    heap_caps_free(bt);
                    heap_caps_free(lt);
                    bt = heap_caps_malloc((size_t)need * sizeof(uint32_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    lt = heap_caps_malloc((size_t)need * sizeof(uint16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    cnt = (bt != NULL && lt != NULL)
                              ? espaperplay_reader_pagen_load(ch, reader_layout_key(), bt, lt,
                                                              need)
                              : 0;
                } else {
                    cnt = 0;
                }
            }
            if (cnt > 0) {
                for (int i = 0; i < cnt; i++) {
                    s_pstarts[i].block = bt[i];
                    s_pstarts[i].line = lt[i];
                }
                s_pstart_cnt = cnt;
                s_ch_total_known = true;
                if (s_ch_pages != NULL && ch >= 0 && ch < s_chapter_cnt &&
                    s_ch_pages[ch] == READER_CH_PAGES_UNSET) {
                    s_ch_pages[ch] = (uint32_t)cnt;
                }
                ESP_LOGI(TAG, "reader: pagination ch %d from cache: %d page(s)", ch + 1, cnt);
                heap_caps_free(bt);
                heap_caps_free(lt);
                return true;
            }
        }
        heap_caps_free(bt);
        heap_caps_free(lt);
    }
    return true;
}

/** pstarts 追加一个页起点。 */
static bool rdr_pstart_push(reader_pos_t pos) {
    if (s_pstart_cnt >= s_pstart_cap) {
        const int nc = s_pstart_cap * 2;
        reader_pos_t *nb = heap_caps_realloc(s_pstarts, (size_t)nc * sizeof(reader_pos_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb == NULL) {
            nb = heap_caps_realloc(s_pstarts, (size_t)nc * sizeof(reader_pos_t), MALLOC_CAP_8BIT);
        }
        if (nb == NULL) {
            return false;
        }
        s_pstarts = nb;
        s_pstart_cap = nc;
    }
    s_pstarts[s_pstart_cnt++] = pos;
    return true;
}

/** 块文本行进给（块内 off 起的一行，返回字节长度；style 用于字体选择）。 */
static uint32_t rdr_next_line(const char *text, size_t text_len, uint32_t off, uint32_t blk_end,
                              lv_font_t *font, lv_text_attributes_t *attr) {
    if (off >= blk_end) {
        return 0;
    }
    return lv_text_get_next_line(&text[off], (uint32_t)(blk_end - off), font, NULL, attr);
}

/**
 * 从 pos 出发填充一页，产出到下一页的起点与页片段表。
 * 返回 false 表示章节已结束（pos 即末页之后的终点）。
 */
static bool reader_paginate_from(reader_pos_t pos, reader_pos_t *next_out, int *seg_cnt) {
    const espaperplay_reader_block_t *blocks = NULL;
    int blk_cnt = 0;
    blocks = espaperplay_reader_blocks(&blk_cnt);
    const char *text = espaperplay_reader_chapter_text(NULL);
    if (blocks == NULL || text == NULL || blk_cnt <= 0 || (int)pos.block >= blk_cnt) {
        return false;
    }

    int nsegs = 0;

    /* 图片块独占一页 */
    if (blocks[pos.block].image >= 0) {
        s_segs[nsegs].blk = NULL;
        s_segs[nsegs].off = 0;
        s_segs[nsegs].len = 0;
        s_segs[nsegs].flags = 0;
        s_segs[nsegs].image = blocks[pos.block].image;
        s_segs[nsegs].y = 0;
        s_segs[nsegs].x = 0;
        s_segs[nsegs].w = s_content_w;
        nsegs++;
        *seg_cnt = nsegs;
        next_out->block = pos.block + 1;
        next_out->line = 0;
        return (int)next_out->block < blk_cnt;
    }

    int y = 0;
    uint32_t b = pos.block;
    uint16_t li = pos.line;
    while ((int)b < blk_cnt && nsegs < READER_SEG_MAX) {
        const espaperplay_reader_block_t *blk = &blocks[b];
        if (blk->image >= 0) {
            break; /* 图片另起一页 */
        }
        if (blk->len == 0) {
            /* 空块：占一行高（段落间距） */
            y += lv_font_get_line_height(s_font) + READER_LINE_SPACE;
            b++;
            li = 0;
            if (y > s_content_h) {
                break;
            }
            continue;
        }

        lv_font_t *font = reader_block_font(blk->flags);
        const int lh = lv_font_get_line_height(font) + READER_LINE_SPACE;
        lv_text_attributes_t attr;
        lv_text_attributes_init(&attr);
        attr.letter_space = 0;
        attr.line_space = READER_LINE_SPACE;
        const bool quote = (blk->flags & ESPAPERPLAY_READER_BLK_QUOTE) != 0;
        attr.max_width = quote ? s_content_w - 2 * READER_QUOTE_INDENT : s_content_w;
        attr.text_flags = LV_TEXT_FLAG_BREAK_ALL;

        const uint32_t blk_end = blk->off + blk->len;
        /* 跳过 li 行（页起点在块中间） */
        uint32_t p = blk->off;
        for (uint16_t k = 0; k < li; k++) {
            const uint32_t adv = rdr_next_line(text, 0, p, blk_end, font, &attr);
            if (adv == 0) {
                break;
            }
            p += adv;
        }
        if (p >= blk_end) {
            b++;
            li = 0;
            continue;
        }

        const uint32_t seg_start = p;
        int lines = 0;
        while (p < blk_end && y + lh <= s_content_h && lines < 512) {
            const uint32_t adv = rdr_next_line(text, 0, p, blk_end, font, &attr);
            if (adv == 0) {
                break; /* 字体缺失等：停止（防御死循环） */
            }
            p += adv;
            lines++;
            y += lh;
        }
        if (lines == 0 && y + lh > s_content_h && (int)b == (int)pos.block && li == pos.line) {
            /* 首行超高（内容区过小）：强制放一行，保证前进 */
            const uint32_t adv = rdr_next_line(text, 0, p, blk_end, font, &attr);
            if (adv > 0) {
                p += adv;
                lines = 1;
            }
        }
        if (lines == 0) {
            break; /* 无法前进
            */
        }
        s_segs[nsegs].blk = blk;
        s_segs[nsegs].off = seg_start;
        s_segs[nsegs].len = p - seg_start;
        s_segs[nsegs].flags = blk->flags;
        s_segs[nsegs].image = -1;
        s_segs[nsegs].y = y - lines * lh;
        s_segs[nsegs].x = quote ? READER_QUOTE_INDENT : 0;
        s_segs[nsegs].w = attr.max_width;
        nsegs++;

        if (p >= blk_end) {
            /* 块完成：段后间距 */
            y += READER_PAR_SPACE;
            b++;
            li = 0;
        } else {
            li = (uint16_t)(li + lines);
            break; /* 页满 */
        }
    }

    *seg_cnt = nsegs;
    next_out->block = b;
    next_out->line = li;
    return (int)b < blk_cnt && nsegs > 0;
}

/** 推进一页边界；返回 false=章节已算完或内存不足（推进停止）。 */
static bool rdr_advance_page(void) {
    if (s_ch_total_known || s_pstarts == NULL) {
        return false;
    }
    reader_pos_t next;
    int seg_cnt = 0;
    int blk_cnt_all = 0;
    const bool stopped = !reader_paginate_from(s_pstarts[s_pstart_cnt - 1], &next, &seg_cnt);
    (void)espaperplay_reader_blocks(&blk_cnt_all);
    /* 只有真正推进到章末才算分页完成；中途卡住（字形缺失等）的残缺表
     * 不能当作完成，更不能落盘（否则下次开书把残缺缓存当有效直用） */
    const bool reached_end = stopped && next.block >= (uint32_t)blk_cnt_all;
    if (stopped) {
        s_ch_total_known = true;
        /* 章节页数已知：记录到 s_pstarts 所属章节 */
        if (reached_end && s_ch_pages != NULL && s_pstart_ch >= 0 &&
            s_pstart_ch < s_chapter_cnt && s_ch_pages[s_pstart_ch] == READER_CH_PAGES_UNSET) {
            s_ch_pages[s_pstart_ch] = (uint32_t)s_pstart_cnt;
        }
        /* 分页缓存异步落盘（字号/内容区变化经 font_key 自动失效） */
        if (reached_end && s_pstart_ch >= 0 && s_pstart_cnt > 1) {
            uint32_t *bt = heap_caps_malloc((size_t)s_pstart_cnt * sizeof(uint32_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            uint16_t *lt = heap_caps_malloc((size_t)s_pstart_cnt * sizeof(uint16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (bt != NULL && lt != NULL) {
                for (int i = 0; i < s_pstart_cnt; i++) {
                    bt[i] = s_pstarts[i].block;
                    lt[i] = s_pstarts[i].line;
                }
                ESP_LOGI(TAG, "reader: pagination ch %d done: %d page(s), %u ms -> cache",
                         s_pstart_ch + 1, s_pstart_cnt,
                         (unsigned)lv_tick_elaps(s_paginate_t0));
                espaperplay_reader_pagen_save_async(s_pstart_ch, reader_layout_key(),
                                                    s_pstart_cnt, bt, lt);
            }
            heap_caps_free(bt);
            heap_caps_free(lt);
        }
        return false;
    }
    if (!rdr_pstart_push(next)) {
        s_ch_total_known = true; /* 内存不足：停止计算 */
        return false;
    }
    if (s_pstart_cnt % READER_SPARSE_STEP == 0) {
        rdr_sparse_add(s_pstart_cnt - 1, s_pstarts[s_pstart_cnt - 1]);
    }
    return true;
}

/** 确保章内第 want 页起点已计算（惰性；含远跳稀疏回退）。 */
static void reader_compute_local(int want) {
    if (s_pstarts == NULL) {
        return;
    }
    /* 远跳回退：目标远超已计算边界时，从最近检查点重算（每次调用至多回退一次；
     * 不能放进推进循环——否则每推进 1 页又回退，25↔26 震荡死循环） */
    const int from = s_pstart_cnt - 1;
    if (want - from > READER_SPARSE_STEP && s_sparse_cnt > 0) {
        const int best = rdr_sparse_find(want);
        if (best >= 0 && s_sparse[best].page < from) {
            s_pstart_cnt = s_sparse[best].page + 1;
        }
    }
    while (!s_ch_total_known && s_pstart_cnt <= want) {
        if (!rdr_advance_page()) {
            if (s_pstart_ch >= 0) {
                ESP_LOGI(TAG, "reader: pagination ch %d/%d done: %d page(s), %u ms (sync)",
                         s_pstart_ch + 1, s_chapter_cnt, s_pstart_cnt,
                         (unsigned)lv_tick_elaps(s_paginate_t0));
            }
            break;
        }
    }
}

/** 时间预算内推进分页（后台计数用；返回章节是否已算完；完成时打进度日志）。 */
static bool reader_compute_budget(uint32_t budget_ms) {
    const uint32_t t0 = lv_tick_get();
    while (!s_ch_total_known) {
        if (!rdr_advance_page()) {
            if (s_pstart_ch >= 0) {
                ESP_LOGI(TAG, "reader: pagination ch %d/%d done: %d page(s), %u ms",
                         s_pstart_ch + 1, s_chapter_cnt, s_pstart_cnt,
                         (unsigned)lv_tick_elaps(s_paginate_t0));
            }
            return true;
        }
        if ((uint32_t)(lv_tick_get() - t0) >= budget_ms) {
            break;
        }
    }
    return s_ch_total_known;
}

/** 驻留章总页数（未知返回 -1）。 */
static int reader_local_total(void) {
    if (s_ch_total_known) {
        return s_pstart_cnt;
    }
    return -1;
}

/** 全局页号：需要前缀章节页数全部已知，否则返回 -1。 */
static int reader_global_page(void) {
    if (s_ch_pages == NULL) {
        return -1;
    }
    uint32_t sum = 0;
    for (int i = 0; i < s_ch && i < s_chapter_cnt; i++) {
        if (s_ch_pages[i] == READER_CH_PAGES_UNSET) {
            return -1;
        }
        sum += s_ch_pages[i];
    }
    return (int)sum + s_local_page;
}

/** 全局总页数（有未知章返回 -1）。 */
static int reader_global_total(void) {
    if (s_ch_pages == NULL || s_chapter_cnt <= 0) {
        return -1;
    }
    uint32_t sum = 0;
    for (int i = 0; i < s_chapter_cnt; i++) {
        if (s_ch_pages[i] == READER_CH_PAGES_UNSET) {
            return -1;
        }
        sum += s_ch_pages[i];
    }
    return (int)sum;
}

/* ------------------------------------------------------------------ */
/* 总页数分片计算（LVGL 定时器，时间预算切片，不阻塞交互）               */
/* ------------------------------------------------------------------ */

/**
 * 计数期间驻留章会被换入换出，但视图章（s_ch）不变；s_pstarts 永远描述
 * 「当前驻留章」（espaperplay_reader_chapter_current()），归属记录在
 * s_pstart_ch。用户翻页 / 换字号会打断计数（s_count_ch 复位），被数到
 * 一半的章节下次重数。
 */
static int s_count_ch = -1;   /*!< 正在数的章节（-1=无） */
static int s_poll_ch = -1;    /*!< 轮询等待的章节 */
static int s_poll_tries = 0;  /*!< 连续未就绪次数（超时走同步兜底） */
#define READER_POLL_MAX 120   /*!< ~6s：预取超时后同步装载兜底 */

/** 视图章是否驻留（计数可能换出）。 */
static bool reader_view_resident(void) {
    return s_pstarts != NULL && espaperplay_reader_chapter_current() == s_ch;
}

/**
 * 确保视图章驻留且页起点就绪（不渲染）。
 *
 * 后台计数会把驻留章换成被数章（epub 单驻留槽），此时 s_pstarts 描述的是
 * 计数章——任何交互翻页前必须先恢复视图章，否则会把计数章的页数/边界
 * 套在视图章上（表现为「点下一页重新加载当前页」）。恢复走章节包缓存 +
 * 分页缓存，命中时毫秒级。
 */
static bool reader_ensure_view_resident(void) {
    if (reader_view_resident()) {
        return true;
    }
    s_count_ch = -1; /* 打断计数：驻留槽让给视图章，稍后自动继续 */
    if (espaperplay_reader_load_chapter(s_ch) != ESP_OK) {
        return false;
    }
    if (!reader_pstarts_init(s_ch)) {
        return false;
    }
    reader_compute_local(s_local_page + 1);
    if (s_local_page >= s_pstart_cnt) {
        s_local_page = s_pstart_cnt > 0 ? s_pstart_cnt - 1 : 0;
    }
    return true;
}

static void reader_total_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_ch_pages == NULL || !s_ready) {
        return;
    }
    /* 阶段 1：视图章驻留且页数未知 → 切片推进 */
    if (reader_view_resident() && s_ch < s_chapter_cnt &&
        s_ch_pages[s_ch] == READER_CH_PAGES_UNSET) {
        (void)reader_compute_budget(8);
    }
    /* 阶段 2：其余未知章逐章数（跨 tick 切片推进；加载一章的开销 ~百 ms，
     * 集中在该 tick 内完成） */
    if (!reader_view_resident() || s_ch_pages[s_ch] != READER_CH_PAGES_UNSET) {
        int next = -1;
        for (int i = 0; i < s_chapter_cnt; i++) {
            if (s_ch_pages[i] == READER_CH_PAGES_UNSET && i != s_ch) {
                next = i;
                break;
            }
        }
        if (next >= 0) {
            if (s_count_ch != next) {
                ESP_LOGD(TAG, "reader: counting ch %d/%d", next + 1, s_chapter_cnt);
                /* 新开一章：轮询装载（worker 异步预取，LVGL 线程零解析）；
                 * 未就绪本 tick 跳过，下一 tick 再试；长时间无结果按坏章计 0 页 */
                if (s_poll_ch != next) {
                    s_poll_ch = next;
                    s_poll_tries = 0;
                }
                if (!espaperplay_reader_poll_chapter(next)) {
                    if (++s_poll_tries < READER_POLL_MAX) {
                        goto footer_tick;
                    }
                    /* 超时（worker 繁忙，如首次打开时逐章写缓存）：同步装载
                     * 兜底，绝不误标 0 页损坏总页数 */
                    ESP_LOGW(TAG, "reader: chapter %d prefetch timeout, sync fallback", next + 1);
                    if (espaperplay_reader_load_chapter(next) == ESP_OK &&
                        reader_pstarts_init(next)) {
                        s_count_ch = next;
                    } else {
                        s_ch_pages[next] = 0; /* 真坏章 */
                    }
                    s_poll_ch = -1;
                    goto footer_tick;
                } else if (espaperplay_reader_chapter_current() != next ||
                           espaperplay_reader_blocks(NULL) == NULL) {
                    /* 装载失败（驻留仍是别章）或解包为空：按坏章计 0 页，
                     * 防止把驻留章页数误记到坏章头上 */
                    s_ch_pages[next] = 0;
                    s_poll_ch = -1;
                } else if (reader_pstarts_init(next)) {
                    s_count_ch = next;
                    s_poll_ch = -1;
                } else {
                    s_count_ch = -1;
                    s_poll_ch = -1;
                }
            }
            if (s_count_ch == next) {
                (void)reader_compute_budget(8);
                if (s_ch_pages[next] != READER_CH_PAGES_UNSET) {
                    s_count_ch = -1; /* 数完：下一 tick 换下一章 */
                }
            }
        } else if (!reader_view_resident()) {
            /* 阶段 3：全部已知 → 恢复视图章驻留 */
            if (reader_ensure_view_resident()) {
                reader_compute_local(s_local_page + 1);
                if (s_local_page >= s_pstart_cnt) {
                    s_local_page = s_pstart_cnt - 1;
                }
            }
        }
    }
footer_tick:;
    const int total = reader_global_total();
    const uint32_t now = lv_tick_get();
    if (total >= 0 || (int32_t)(now - s_last_footer_tick) >= 300) {
        s_last_footer_tick = now;
        reader_footer_update();
    }
    if (total >= 0 && reader_view_resident() && s_total_timer != NULL) {
        lv_timer_delete(s_total_timer);
        s_total_timer = NULL;
        ESP_LOGI(TAG, "reader: total pages = %d", total);
    }
}

static void reader_total_task_start(void) {
    if (s_total_timer != NULL || s_ch_pages == NULL || reader_global_total() >= 0) {
        return;
    }
    s_last_footer_tick = lv_tick_get();
    s_total_timer = lv_timer_create(reader_total_timer_cb, 48, NULL);
}

/* ------------------------------------------------------------------ */
/* 加载中弹窗                                                           */
/* ------------------------------------------------------------------ */

static void reader_loading_close(void) {
    if (s_loading_modal != NULL) {
        lv_obj_del(s_loading_modal);
        s_loading_modal = NULL;
    }
    s_loading = false;
}

static void reader_loading_open(void) {
    if (s_loading_modal != NULL) {
        return;
    }
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    reader_screen_size(&scr_w, &scr_h);
    s_loading_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_loading_modal, scr_w, scr_h);
    lv_obj_set_pos(s_loading_modal, 0, 0);
    lv_obj_set_style_bg_opa(s_loading_modal, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_loading_modal, 0, 0);
    lv_obj_set_style_radius(s_loading_modal, 0, 0);
    lv_obj_set_style_pad_all(s_loading_modal, 0, 0);
    lv_obj_remove_flag(s_loading_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_loading_modal, LV_OBJ_FLAG_CLICKABLE);

    const int card_w = 200;
    const int card_h = 100;
    lv_obj_t *card = lv_obj_create(s_loading_modal);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, "加载中…");
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(label, s_font, 0);
    }
    lv_obj_center(label);
    s_loading = true;
}

/* ------------------------------------------------------------------ */
/* 页渲染                                                               */
/* ------------------------------------------------------------------ */

/** 渲染章内当前页（分页 → 片段 → 控件）。 */
static void reader_render_page(void) {
    if (s_content == NULL || s_pstarts == NULL) {
        return;
    }
    reader_compute_local(s_local_page + 1);
    if (s_local_page >= s_pstart_cnt) {
        s_local_page = s_pstart_cnt - 1;
        if (s_local_page < 0) {
            s_local_page = 0;
        }
    }

    lv_obj_clean(s_content);
    bool page_has_image = false; /* 本页含插图（已就绪显示）：灰度刷新判定用 */
    reader_pos_t next;
    int seg_cnt = 0;
    const bool has_more = reader_paginate_from(s_pstarts[s_local_page], &next, &seg_cnt);

    const char *text = espaperplay_reader_chapter_text(NULL);
    for (int i = 0; i < seg_cnt; i++) {
        const reader_seg_t *sg = &s_segs[i];
        if (sg->image >= 0) {
            /* 图片页：按 2 倍内容区预算解码（抽样步长小、留足缩放质量），
             * 再等比缩放到恰好内接内容区并居中。 */
            const lv_image_dsc_t *dsc = NULL;
            const int budget_w = s_content_w * 2;
            const int budget_h = s_content_h * 2;
            const esp_err_t err = espaperplay_reader_image(sg->image, budget_w, budget_h, &dsc);
            if (err == ESP_OK && dsc != NULL && dsc->header.w > 0 && dsc->header.h > 0) {
                const int iw = dsc->header.w;
                const int ih = dsc->header.h;
                int tw = iw;
                int th = ih;
                if (tw > s_content_w) {
                    tw = s_content_w;
                    th = (int)((int64_t)ih * s_content_w / iw);
                }
                if (th > s_content_h) {
                    th = s_content_h;
                    tw = (int)((int64_t)iw * s_content_h / ih);
                }
                lv_obj_t *img = lv_image_create(s_content);
                lv_image_set_src(img, dsc);
                lv_image_set_pivot(img, iw / 2, ih / 2);
                lv_image_set_scale(img, (uint32_t)((int64_t)tw * 256 / iw));
                lv_obj_set_size(img, tw, th);
                lv_obj_center(img);
                page_has_image = true;
            } else {
                lv_obj_t *lbl = lv_label_create(s_content);
                lv_label_set_text(lbl, "[图片]");
                lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
                lv_obj_set_style_text_font(lbl, s_font, 0);
                lv_obj_center(lbl);
            }
            continue;
        }
        if (sg->len == 0 || text == NULL) {
            continue;
        }
        lv_obj_t *lbl = lv_label_create(s_content);
        /* 原地 NUL 结尾化（label 内部复制文本，随即恢复） */
        char *spot = (char *)&text[sg->off + sg->len];
        const char saved = *spot;
        *spot = '\0';
        lv_label_set_text(lbl, &text[sg->off]);
        *spot = saved;
        lv_obj_set_pos(lbl, sg->x, sg->y);
        lv_obj_set_width(lbl, sg->w);
        /* 段落对齐（CSS text-align / align 属性） */
        if ((sg->flags & ESPAPERPLAY_READER_BLK_CENTER) != 0) {
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        } else if ((sg->flags & ESPAPERPLAY_READER_BLK_RIGHT) != 0) {
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
        }
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_set_style_pad_all(lbl, 0, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);
        lv_font_t *font = reader_block_font(sg->flags);
        if (font != NULL) {
            lv_obj_set_style_text_font(lbl, font, 0);
        }
    }
    (void)has_more;

    /* 插图页自动灰度：延后一拍触发（当前帧 BW 局刷先完成，随后对已显示的
     * 插图执行一次 GRAY4 全屏灰阶刷新，避免拍到中间帧）。 */
    if (page_has_image && espaperplay_system_get_reader_img_gray4()) {
        if (s_gray4_timer == NULL) {
            s_gray4_timer = lv_timer_create(reader_gray4_deferred_cb, 60, NULL);
            lv_timer_set_repeat_count(s_gray4_timer, 1);
        }
    }
}

/** 刷新页码显示（底部 + 底边栏；文本未变化时不触发重绘）。 */
static void reader_footer_update(void) {
    char buf[48];
    const int total = reader_global_total();
    const int gcur = reader_global_page();
    if (total >= 0 && gcur >= 0) {
        snprintf(buf, sizeof(buf), "%d / %d", gcur + 1, total);
    } else if (gcur >= 0) {
        snprintf(buf, sizeof(buf), "%d / …", gcur + 1);
    } else {
        const int lt = reader_local_total();
        if (lt >= 0) {
            snprintf(buf, sizeof(buf), "%d / %d", s_local_page + 1, lt);
        } else {
            snprintf(buf, sizeof(buf), "%d / …", s_local_page + 1);
        }
    }
    if (strcmp(s_footer_last, buf) == 0) {
        return; /* 文本未变化：跳过，避免墨水屏无谓局刷 */
    }
    strlcpy(s_footer_last, buf, sizeof(s_footer_last));
    if (s_page_label != NULL) {
        lv_label_set_text(s_page_label, buf);
    }
    if (s_bar_page_label != NULL) {
        lv_label_set_text(s_bar_page_label, buf);
    }
}

/** 显示指定（章节, 章内页）。 */
static void reader_show_page(int ch, int local) {
    if (!s_ready || s_ch_pages == NULL) {
        return;
    }
    reader_gray4_mode_set(false);

    if (ch < 0) {
        ch = 0;
    }
    if (ch >= s_chapter_cnt) {
        ch = s_chapter_cnt - 1;
    }
    if (ch != s_ch || !reader_view_resident()) {
        s_count_ch = -1; /* 打断后台计数 */
        const uint32_t t0 = lv_tick_get();
        const esp_err_t lerr = espaperplay_reader_load_chapter(ch);
        if (lerr != ESP_OK) {
            ESP_LOGW(TAG, "reader: chapter %d load failed (%s), %u ms", ch + 1, esp_err_to_name(lerr),
                     (unsigned)lv_tick_elaps(t0));
            return;
        }
        ESP_LOGI(TAG, "reader: chapter %d loaded in %u ms", ch + 1,
                 (unsigned)lv_tick_elaps(t0));
        s_ch = ch;
        if (!reader_pstarts_init(ch)) {
            lv_label_set_text(s_hint_label, "内存不足");
            lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        s_local_page = 0;
    }
    if (local < 0) {
        local = 0;
    }
    s_local_page = local;
    reader_render_page();
    reader_footer_update();
    ESP_LOGI(TAG, "reader: chapter %d page %d", s_ch + 1, s_local_page + 1);
}

static void reader_next_page(void) {
    if (!s_ready) {
        return;
    }
    if (!reader_ensure_view_resident()) {
        return; /* 恢复失败（罕见）：本次忽略，下次重试 */
    }
    const int total = reader_local_total(); /* -1=本章分页未完成 */
    if (total < 0) {
        /* 分页未完成：只要下一页边界已算出就直接翻；算不出时尝试推进一步
         * （单页测量，~几 ms），仍不可得则下一 tick 由计数定时器补齐 */
        if (s_local_page + 1 < s_pstart_cnt) {
            reader_show_page(s_ch, s_local_page + 1);
            return;
        }
        if (rdr_advance_page() && s_local_page + 1 < s_pstart_cnt) {
            reader_show_page(s_ch, s_local_page + 1);
            return;
        }
        /* 本章边界暂时到头：翻到下一章开头（预取通常已就绪）；没有下一章
         * 则确实已是最后一页 */
        if (s_ch + 1 < s_chapter_cnt) {
            ESP_LOGI(TAG, "reader: next chapter (pagination in progress, %d page(s) so far)",
                     s_pstart_cnt);
            reader_show_page(s_ch + 1, 0);
        } else if (s_ch_total_known) {
            ESP_LOGI(TAG, "reader: already last page");
        } else {
            ESP_LOGD(TAG, "reader: at computed end, waiting for pagination");
        }
        return;
    }
    /* 分页已完成：精确判断 */
    if (s_local_page + 1 < total) {
        reader_show_page(s_ch, s_local_page + 1);
        return;
    }
    if (s_ch + 1 < s_chapter_cnt) {
        reader_show_page(s_ch + 1, 0);
    } else {
        ESP_LOGI(TAG, "reader: already last page");
    }
}

static void reader_prev_page(void) {
    if (!s_ready) {
        return;
    }
    if (!reader_ensure_view_resident()) {
        return;
    }
    if (s_local_page > 0) {
        reader_show_page(s_ch, s_local_page - 1);
        return;
    }
    if (s_ch > 0) {
        /* 上一章末页（需全量分页该章；切换章会打断后台计数） */
        s_count_ch = -1;
        if (espaperplay_reader_load_chapter(s_ch - 1) == ESP_OK) {
            s_ch--;
            if (!reader_pstarts_init(s_ch)) {
                return;
            }
            reader_compute_local(1 << 20);
            reader_show_page(s_ch, s_pstart_cnt - 1);
            ESP_LOGI(TAG, "reader: pagination ch %d done: %d page(s), %u ms (prev-chapter)",
                     s_ch + 1, s_pstart_cnt, (unsigned)lv_tick_elaps(s_paginate_t0));
        }
    } else {
        ESP_LOGI(TAG, "reader: already first page");
    }
}

/* ------------------------------------------------------------------ */
/* 字号 / 灰阶                                                          */
/* ------------------------------------------------------------------ */

/** 字号变化：全部章页数失效并后台重算；保持（章, 章内页，钳制）。 */
static void reader_set_font(int idx) {
    if (idx < 0 || idx >= READER_FONT_CNT || idx == s_font_idx) {
        return;
    }
    s_font_idx = idx;
    s_font = reader_font_styled(reader_base_size(), ESPAPERPLAY_FONT_STYLE_NORMAL);
    if (s_content != NULL && s_font != NULL) {
        /* 正文字体作用于空块行高与提示 */
    }
    if (s_ch_pages != NULL) {
        for (int i = 0; i < s_chapter_cnt; i++) {
            s_ch_pages[i] = READER_CH_PAGES_UNSET;
        }
    }
    if (!reader_ensure_view_resident()) {
        return; /* 重载失败：放弃重排（保持旧渲染） */
    }
    if (s_pstarts != NULL) {
        if (!reader_pstarts_init(s_ch)) {
            return;
        }
        reader_compute_local(s_local_page + 1);
        if (s_local_page >= s_pstart_cnt) {
            s_local_page = s_pstart_cnt - 1;
        }
    }
    if (s_ready) {
        reader_render_page();
        reader_footer_update();
        reader_total_task_start();
    }
}

/**
 * 设置/退出「灰度显示」模式（详见 espaperplay_gui 的全屏基线清残留机制）。
 */
static void reader_gray4_mode_set(bool on) {
    if (s_gray4_display == on) {
        return;
    }
    s_gray4_display = on;
    espaperplay_ui_status_bar_set_suspended(on);
}

/** 单次 GRAY4 刷屏。 */
static void reader_do_gray4_once(void) {
    ESP_LOGI(TAG, "reader: single GRAY4 refresh");
    (void)espaperplay_gui_show_gray4();
    (void)espaperplay_gui_set_color(ESPAPERPLAY_GUI_COLOR_BW);
    reader_gray4_mode_set(true);
}

/* ------------------------------------------------------------------ */
/* 底边栏                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    READER_BAR_PREV = 0,
    READER_BAR_NEXT,
    READER_BAR_JUMP,
    READER_BAR_TOC,
    READER_BAR_FONT_DOWN,
    READER_BAR_FONT_UP,
    READER_BAR_GRAY4,
    READER_BAR_BACK,
} reader_bar_action_t;

/* ---- 目录覆盖层 ---- */
#define READER_TOC_ROW_H 38
#define READER_TOC_MAX_ROWS 12
static lv_obj_t *s_toc_overlay = NULL;   /*!< 目录全屏覆盖层（NULL=未打开） */
static lv_obj_t *s_toc_rows[READER_TOC_MAX_ROWS];
static int s_toc_row_ch[READER_TOC_MAX_ROWS]; /*!< 行 → 章节号（-1=空行） */
static int s_toc_row_cnt = 0;
static lv_obj_t *s_toc_page_label = NULL;
static lv_obj_t *s_toc_prev = NULL;
static lv_obj_t *s_toc_next = NULL;
static int s_toc_page = 0;
static int s_toc_page_cnt = 1;
static int s_toc_per_page = 8;
static int s_toc_row_w = 0;       /*!< 行宽（打开时算术求得；新面板坐标未布局前读宽为 0） */

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
    case READER_BAR_TOC:
        reader_bar_close();
        reader_toc_open();
        break;
    case READER_BAR_FONT_DOWN:
        reader_set_font(s_font_idx - 1);
        break;
    case READER_BAR_FONT_UP:
        reader_set_font(s_font_idx + 1);
        break;
    case READER_BAR_GRAY4:
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

static void reader_bar_overlay_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    reader_bar_close();
}

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

    reader_gray4_mode_set(false);

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

    int y = pad;
    const int w1 = (pw - 2 * pad - 2 * gap) / 3;
    reader_bar_button(s_bar_panel, "◀", pad, y, w1, bh, READER_BAR_PREV);
    s_bar_page_label =
        reader_bar_button(s_bar_panel, "", pad + w1 + gap, y, w1, bh, READER_BAR_JUMP);
    reader_bar_button(s_bar_panel, "▶", pad + 2 * (w1 + gap), y, w1, bh, READER_BAR_NEXT);
    y += bh + gap;

    /* 行 2：目录 / A- / A+ / 灰度 / 返回（5 键） */
    const int w2 = (pw - 2 * pad - 4 * gap) / 5;
    int x = pad;
    reader_bar_button(s_bar_panel, "目录", x, y, w2, bh, READER_BAR_TOC);
    x += w2 + gap;
    reader_bar_button(s_bar_panel, "A-", x, y, w2, bh, READER_BAR_FONT_DOWN);
    x += w2 + gap;
    reader_bar_button(s_bar_panel, "A+", x, y, w2, bh, READER_BAR_FONT_UP);
    x += w2 + gap;
    reader_bar_button(s_bar_panel, "灰度", x, y, w2, bh, READER_BAR_GRAY4);
    x += w2 + gap;
    reader_bar_button(s_bar_panel, "返回", x, y, w2, bh, READER_BAR_BACK);

    reader_footer_update();
    ESP_LOGI(TAG, "reader: bottom bar open");
}

static void reader_bar_toggle(void) {
    if (s_bar_overlay != NULL) {
        reader_bar_close();
    } else {
        reader_bar_open();
    }
}

/* ------------------------------------------------------------------ */
/* 目录覆盖层（分页列表 + 点击跳转）                                      */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_toc_panel = NULL; /*!< 目录白底面板（行挂载点） */

/** 行点击：关闭目录并跳转到对应章首。 */
static void reader_toc_row_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    const int ch = (int)(intptr_t)lv_event_get_user_data(e);
    if (ch < 0 || ch >= espaperplay_reader_chapter_count()) {
        return;
    }
    reader_toc_close();
    reader_bar_close();
    reader_show_page(ch, 0);
    ESP_LOGI(TAG, "reader: toc jump -> chapter %d", ch + 1);
}

/** 按 s_toc_page 重建目录行（面板已存在）。 */
static void reader_toc_build_rows(void) {
    /* 清旧行 */
    for (int i = 0; i < s_toc_row_cnt; i++) {
        if (s_toc_rows[i] != NULL) {
            lv_obj_del(s_toc_rows[i]);
            s_toc_rows[i] = NULL;
        }
        s_toc_row_ch[i] = -1;
    }
    s_toc_row_cnt = 0;

    const int total = espaperplay_reader_chapter_count();
    const int row_w = s_toc_row_w; /* 布局前面板读宽为 0，须用算术值 */
    for (int i = 0; i < s_toc_per_page; i++) {
        const int ch = s_toc_page * s_toc_per_page + i;
        if (ch >= total) {
            break;
        }
        char title[96];
        char text[128];
        if (espaperplay_reader_toc_title(ch, title, sizeof(title))) {
            snprintf(text, sizeof(text), "%d. %s", ch + 1, title);
        } else {
            snprintf(text, sizeof(text), "%d. 第 %d 章", ch + 1, ch + 1);
        }
        lv_obj_t *row = lv_button_create(s_toc_panel);
        lv_obj_set_size(row, row_w, READER_TOC_ROW_H - 8);
        lv_obj_set_pos(row, 12, 36 + i * READER_TOC_ROW_H);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(lbl, s_font, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, row_w - 8);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(row, reader_toc_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ch);
        s_toc_rows[i] = row;
        s_toc_row_ch[i] = ch;
        s_toc_row_cnt = i + 1;
    }

    /* 页码与翻页按钮状态 */
    char pl[24];
    snprintf(pl, sizeof(pl), "%d / %d", s_toc_page + 1, s_toc_page_cnt);
    lv_label_set_text(s_toc_page_label, pl);
    lv_label_set_text(lv_obj_get_child(s_toc_prev, 0), s_toc_page > 0 ? "◀" : "─");
    lv_label_set_text(lv_obj_get_child(s_toc_next, 0), s_toc_page + 1 < s_toc_page_cnt ? "▶" : "─");
}

/** 目录翻页按钮。 */
static void reader_toc_page_btn_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    const intptr_t dir = e != NULL ? (intptr_t)lv_event_get_user_data(e) : 0;
    const int np = s_toc_page + (int)dir;
    if (np < 0 || np >= s_toc_page_cnt) {
        return;
    }
    s_toc_page = np;
    reader_toc_build_rows();
}

static void reader_toc_overlay_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    reader_toc_close();
}

static void reader_toc_close(void) {
    if (s_toc_overlay != NULL) {
        lv_obj_del(s_toc_overlay);
        s_toc_overlay = NULL;
    }
    s_toc_panel = NULL;
    for (int i = 0; i < READER_TOC_MAX_ROWS; i++) {
        s_toc_rows[i] = NULL;
        s_toc_row_ch[i] = -1;
    }
    s_toc_row_cnt = 0;
    s_toc_page_label = NULL;
    s_toc_prev = NULL;
    s_toc_next = NULL;
    s_toc_page = 0;
}

/** 打开目录覆盖层（底边栏触发；行点击跳章首，自动定位到当前章所在页）。 */
static void reader_toc_open(void) {
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    reader_screen_size(&scr_w, &scr_h);
    const int total = espaperplay_reader_chapter_count();
    if (total <= 0) {
        return;
    }

    reader_gray4_mode_set(false);

    /* 全屏透明覆盖层：点空白关闭 */
    s_toc_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_toc_overlay, scr_w, scr_h);
    lv_obj_set_pos(s_toc_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_toc_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_toc_overlay, 0, 0);
    lv_obj_set_style_radius(s_toc_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_toc_overlay, 0, 0);
    lv_obj_remove_flag(s_toc_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toc_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_toc_overlay, reader_toc_overlay_cb, LV_EVENT_CLICKED, NULL);

    /* 白底面板（状态栏与页码之间） */
    const int margin = READER_MARGIN;
    const int pw = scr_w - 2 * margin;
    const int ph = scr_h - READER_STATUS_H - READER_FOOTER_H - 12;
    s_toc_row_w = pw - 24; /* 行左右各留 12；此刻面板坐标未布局，不可读宽 */
    s_toc_panel = lv_obj_create(s_toc_overlay);
    lv_obj_set_size(s_toc_panel, pw, ph);
    lv_obj_set_pos(s_toc_panel, margin, READER_STATUS_H + 6);
    lv_obj_set_style_bg_color(s_toc_panel, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_toc_panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_toc_panel, 2, 0);
    lv_obj_set_style_radius(s_toc_panel, 12, 0);
    lv_obj_set_style_pad_all(s_toc_panel, 0, 0);
    lv_obj_remove_flag(s_toc_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_toc_panel);
    lv_label_set_text(title, "目录");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(title, s_font, 0);
    }
    lv_obj_set_pos(title, 12, 8);

    /* 每页行数由面板高度决定 */
    const int list_top = 34;
    const int nav_h = 44;
    s_toc_per_page = (ph - list_top - nav_h) / READER_TOC_ROW_H;
    if (s_toc_per_page < 1) {
        s_toc_per_page = 1;
    }
    if (s_toc_per_page > READER_TOC_MAX_ROWS) {
        s_toc_per_page = READER_TOC_MAX_ROWS;
    }
    s_toc_page_cnt = (total + s_toc_per_page - 1) / s_toc_per_page;
    s_toc_page = s_ch / s_toc_per_page; /* 定位到当前章所在页 */
    if (s_toc_page >= s_toc_page_cnt) {
        s_toc_page = s_toc_page_cnt - 1;
    }

    /* 底部导航：◀ 页码 ▶ */
    const int nav_y = ph - nav_h;
    const int bw = 64;
    s_toc_prev = lv_button_create(s_toc_panel);
    lv_obj_set_size(s_toc_prev, bw, 36);
    lv_obj_set_pos(s_toc_prev, 12, nav_y);
    lv_obj_set_style_bg_color(s_toc_prev, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_toc_prev, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_toc_prev, 2, 0);
    lv_obj_set_style_radius(s_toc_prev, 8, 0);
    lv_obj_set_style_shadow_width(s_toc_prev, 0, 0);
    lv_obj_t *pl1 = lv_label_create(s_toc_prev);
    lv_label_set_text(pl1, "◀");
    lv_obj_set_style_text_color(pl1, lv_color_black(), 0);
    lv_obj_set_style_text_font(pl1, s_font, 0);
    lv_obj_center(pl1);
    lv_obj_add_event_cb(s_toc_prev, reader_toc_page_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);

    s_toc_page_label = lv_label_create(s_toc_panel);
    char pl[24];
    snprintf(pl, sizeof(pl), "%d / %d", s_toc_page + 1, s_toc_page_cnt);
    lv_label_set_text(s_toc_page_label, pl);
    lv_obj_set_style_text_color(s_toc_page_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_toc_page_label, s_font, 0);
    lv_obj_set_width(s_toc_page_label, pw - 2 * bw - 32);
    lv_obj_set_style_text_align(s_toc_page_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_toc_page_label, 12 + bw + 4, nav_y + 8);

    s_toc_next = lv_button_create(s_toc_panel);
    lv_obj_set_size(s_toc_next, bw, 36);
    lv_obj_set_pos(s_toc_next, pw - 12 - bw, nav_y);
    lv_obj_set_style_bg_color(s_toc_next, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_toc_next, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_toc_next, 2, 0);
    lv_obj_set_style_radius(s_toc_next, 8, 0);
    lv_obj_set_style_shadow_width(s_toc_next, 0, 0);
    lv_obj_t *pl2 = lv_label_create(s_toc_next);
    lv_label_set_text(pl2, "▶");
    lv_obj_set_style_text_color(pl2, lv_color_black(), 0);
    lv_obj_set_style_text_font(pl2, s_font, 0);
    lv_obj_center(pl2);
    lv_obj_add_event_cb(s_toc_next, reader_toc_page_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)+1);

    reader_toc_build_rows();
    ESP_LOGI(TAG, "reader: toc open (%d chapter(s), %d page(s))", total, s_toc_page_cnt);
}

/* ------------------------------------------------------------------ */
/* 跳转输入模态                                                         */
/* ------------------------------------------------------------------ */

static void reader_jump_close(void) {
    if (s_jump_modal != NULL) {
        lv_obj_del(s_jump_modal);
        s_jump_modal = NULL;
    }
    s_jump_ta = NULL;
}

/** 跳转 确定：全局页号 →（章, 章内页）；需总页数已知（后台计数补齐）。 */
static void reader_jump_ok_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_jump_ta == NULL) {
        return;
    }
    const char *txt = lv_textarea_get_text(s_jump_ta);
    long p = strtol(txt, NULL, 10);
    reader_jump_close();
    reader_bar_close();
    if (p < 1) {
        return;
    }
    if (s_ch_pages == NULL) {
        return;
    }
    const int total = reader_global_total();
    if (total <= 0) {
        /* 页数统计中：跳过（底栏页码 / 跳转模态提示“统计中”） */
        reader_footer_update();
        return;
    }
    if (p > total) {
        p = total;
    }
    uint32_t cum = 0;
    int ch = s_chapter_cnt - 1;
    int local = 0;
    for (int i = 0; i < s_chapter_cnt; i++) {
        const uint32_t cnt = s_ch_pages[i]; /* total 已知 → 全部有效 */
        if ((uint32_t)(p - 1) < cum + cnt) {
            ch = i;
            local = (int)((uint32_t)(p - 1) - cum);
            break;
        }
        cum += cnt;
    }
    /* 无需末页钳制：p ≤ total 时循环已给出正确章内页号（p == total 即末页） */
    reader_show_page(ch, local);
}

static void reader_jump_cancel_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    reader_jump_close();
}

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
    lv_obj_set_style_bg_opa(s_jump_modal, LV_OPA_TRANSP, 0);
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
    lv_obj_set_style_anim_duration(s_jump_ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(s_jump_ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    lv_obj_t *status = lv_label_create(panel);
    char hint[48];
    const int total = reader_global_total();
    if (total > 0) {
        snprintf(hint, sizeof(hint), "请输入 1 - %d", total);
    } else {
        snprintf(hint, sizeof(hint), "页码统计中，请稍后");
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

/**
 * 恢复历史进度：历史记录存储打包位置（章 << 20 | 章内页），恢复无需预数
 * 前缀章节页数（直接加载目标章并惰性分页到目标页）；旧格式（纯页号，
 * 章=0，TXT 兼容）等价解包。总页数由后台定时器补齐。
 */
static void reader_restore_packed(uint32_t packed) {
    int ch = (int)(packed >> 20);
    int local = (int)(packed & 0xFFFFF);
    if (ch >= s_chapter_cnt) {
        ch = s_chapter_cnt > 0 ? s_chapter_cnt - 1 : 0;
    }
    if (ch < 0) {
        ch = 0;
    }
    reader_show_page(ch, local); /* local 越界由渲染惰性钳制 */
}

static void reader_loading_timer_cb(lv_timer_t *t) {
    lv_timer_delete(t);
    s_loading_timer = NULL;
    if (!espaperplay_reader_is_open() || s_chapter_cnt <= 0) {
        reader_loading_close();
        return;
    }
    /* 恢复进度：加载目标章并惰性分页（无需预数前缀章；首章由 restore 加载） */
    s_ch = 0;
    s_local_page = 0;
    s_pstarts = NULL;
    s_pstart_ch = -1;
    s_ready = true;
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    reader_restore_packed(s_pending_start);
    if (s_pstarts == NULL) {
        /* 目标章加载失败：退回首章 */
        reader_show_page(0, 0);
        if (s_pstarts == NULL) {
            reader_loading_close();
            lv_label_set_text(s_hint_label, "打开失败");
            lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_page_label, "0 / 0");
            ESP_LOGE(TAG, "reader: first chapter init failed");
            return;
        }
    }
    reader_total_task_start();
    reader_loading_close();
    ESP_LOGI(TAG, "reader: loading done, start %u (ch %u, page %u)",
             (unsigned)s_pending_start + 1, (unsigned)(s_pending_start >> 20) + 1,
             (unsigned)(s_pending_start & 0xFFFFF) + 1);
}

/** 阅读视图构建（页面 enter：屏幕已由页面栈清空）。 */
static void reader_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t scr_w = 0;
    int32_t scr_h = 0;
    reader_screen_size(&scr_w, &scr_h);

    /* 标题：书名（EPUB dc:title / TXT 文件名） */
    char bar_title[40];
    const char *title = espaperplay_reader_get_title();
    if (title != NULL && title[0] != '\0') {
        size_t tl = strlen(title);
        if (tl > sizeof(bar_title) - 4) {
            tl = sizeof(bar_title) - 4;
            while (tl > 0 && (((unsigned char)title[tl] & 0xC0) == 0x80)) {
                tl--;
            }
            memcpy(bar_title, title, tl);
            bar_title[tl] = '\0';
            strlcat(bar_title, "…", sizeof(bar_title));
        } else {
            strlcpy(bar_title, title, sizeof(bar_title));
        }
    } else {
        strlcpy(bar_title, "阅读器", sizeof(bar_title));
    }
    s_bar = espaperplay_ui_status_bar_create(scr, READER_STATUS_H, bar_title, false);
    espaperplay_ui_status_bar_refresh(s_bar);

    s_content_w = scr_w - 2 * READER_MARGIN;
    s_content_h = scr_h - READER_STATUS_H - READER_FOOTER_H - 2 * READER_MARGIN;
    if (s_content_h < 100) {
        s_content_h = 100;
    }
    s_content_y = READER_STATUS_H + READER_MARGIN;

    (void)espaperplay_gui_set_color(ESPAPERPLAY_GUI_COLOR_BW);

    /* 正文容器（页片段挂载点；渲染时 clean 重建） */
    s_content = lv_obj_create(scr);
    lv_obj_set_pos(s_content, READER_MARGIN, s_content_y);
    lv_obj_set_size(s_content, s_content_w, s_content_h);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    s_font = reader_font_styled(reader_base_size(), ESPAPERPLAY_FONT_STYLE_NORMAL);

    /* 底部页码 */
    s_page_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_page_label, lv_color_black(), 0);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(s_page_label, s_font, 0);
    }
    lv_obj_set_style_text_align(s_page_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_page_label, LV_PCT(100));
    lv_obj_set_pos(s_page_label, 0, scr_h - READER_FOOTER_H + 6);
    lv_label_set_text(s_page_label, "加载中…");

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

    /* 状态复位 */
    s_ch = 0;
    s_local_page = 0;
    s_pstarts = NULL;
    s_pstart_cnt = 0;
    s_pstart_ch = -1;
    s_count_ch = -1;
    s_bar_overlay = NULL;
    s_bar_panel = NULL;
    s_bar_page_label = NULL;
    s_jump_modal = NULL;
    s_jump_ta = NULL;
    s_toc_overlay = NULL;
    s_toc_panel = NULL;
    s_gray4_timer = NULL;
    s_loading_modal = NULL;
    s_loading_timer = NULL;
    s_loading = false;
    s_pending_start = 0;
    s_ch_zero_pstart = false;
    s_footer_last[0] = '\0';
    reader_gray4_mode_set(false);

    /* 章页数表 */
    s_chapter_cnt = espaperplay_reader_chapter_count();
    if (s_ch_pages != NULL) {
        heap_caps_free(s_ch_pages);
        s_ch_pages = NULL;
    }
    if (s_chapter_cnt > 0) {
        s_ch_pages = heap_caps_calloc(s_chapter_cnt, sizeof(uint32_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_ch_pages != NULL) {
            for (int i = 0; i < s_chapter_cnt; i++) {
                s_ch_pages[i] = READER_CH_PAGES_UNSET;
            }
        }
    }

    if (!espaperplay_reader_is_open() || s_chapter_cnt <= 0 || s_ch_pages == NULL) {
        const char *hint = espaperplay_reader_is_open() ? "空文件" : "无文档";
        if (s_chapter_cnt > 0 && s_ch_pages == NULL) {
            hint = "内存不足";
        }
        lv_label_set_text(s_hint_label, hint);
        lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_page_label, "0 / 0");
        s_ready = true;
        ESP_LOGI(TAG, "reader: no document");
        s_touch_down = false;
        return;
    }

    /* 有文档：先显示"加载中"，重量级分页推迟到下一帧 */
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    uint32_t start = 0;
    const char *path = espaperplay_reader_get_path();
    if (path != NULL) {
        espaperplay_reader_history_entry_t e;
        if (espaperplay_reader_history_find(path, &e) == ESP_OK) {
            start = e.page;
        }
    }
    s_pending_start = start;
    reader_loading_open();
    s_loading_timer = lv_timer_create(reader_loading_timer_cb, 30, NULL);
    lv_timer_set_repeat_count(s_loading_timer, 1);

    s_touch_down = false;
    ESP_LOGI(TAG, "reader view entered (%d chapter(s)), start page %u", s_chapter_cnt,
             (unsigned)start + 1);
}

/** 阅读视图退出：保存进度、停后台任务、释放资源。 */
static void reader_exit(void) {
    if (s_ready && espaperplay_reader_is_open()) {
        const char *path = espaperplay_reader_get_path();
        const int gtotal = reader_global_total();
        if (path != NULL) {
            /* 进度存打包位置（章 << 20 | 章内页）：恢复无需预数前缀章节 */
            const uint32_t page = ((uint32_t)s_ch << 20) | (uint32_t)s_local_page;
            const uint32_t total = gtotal >= 0 ? (uint32_t)gtotal : 0;
            (void)espaperplay_reader_history_update(path, page, total);
            ESP_LOGI(TAG, "reader: save progress ch %u page %u (global total %d)",
                     (unsigned)s_ch + 1, (unsigned)s_local_page + 1, gtotal);
        }
    }
    if (s_total_timer != NULL) {
        lv_timer_delete(s_total_timer);
        s_total_timer = NULL;
    }
    if (s_loading_timer != NULL) {
        lv_timer_delete(s_loading_timer);
        s_loading_timer = NULL;
    }
    reader_loading_close();
    reader_gray4_mode_set(false);
    reader_pstarts_free();
    if (s_ch_pages != NULL) {
        heap_caps_free(s_ch_pages);
        s_ch_pages = NULL;
    }
    s_chapter_cnt = 0;
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
    reader_toc_close();
    if (s_gray4_timer != NULL) {
        lv_timer_delete(s_gray4_timer);
        s_gray4_timer = NULL;
    }
    s_bar = NULL;
    s_content = NULL;
    s_page_label = NULL;
    s_hint_label = NULL;
    s_ready = false;
    s_touch_down = false;
    ESP_LOGI(TAG, "reader view exited");
}

/* ------------------------------------------------------------------ */
/* 按键 / 触摸                                                          */
/* ------------------------------------------------------------------ */

static void reader_on_key(const espaperplay_input_event_t *event) {
    if (event->type != ESPAPERPLAY_INPUT_EVENT_KEY) {
        return;
    }
    if (s_loading) {
        return;
    }
    switch (event->key_action) {
    case ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK:
        if (s_toc_overlay != NULL) {
            reader_toc_close();
        } else if (s_jump_modal != NULL) {
            reader_jump_close();
        } else if (s_bar_overlay != NULL) {
            reader_bar_close();
        } else {
            reader_next_page();
        }
        break;
    case ESPAPERPLAY_INPUT_KEY_ACTION_DOUBLE_CLICK:
        if (s_bar_overlay == NULL && s_jump_modal == NULL && s_toc_overlay == NULL) {
            reader_prev_page();
        }
        break;
    case ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_START:
        if (s_bar_overlay == NULL && s_jump_modal == NULL && s_toc_overlay == NULL) {
            reader_do_gray4_once();
        }
        break;
    default:
        break;
    }
}

static void reader_on_touch(const espaperplay_input_event_t *event) {
    if (s_loading) {
        return;
    }
    lv_point_t p;
    espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

    if (s_bar_overlay != NULL || s_jump_modal != NULL || s_toc_overlay != NULL) {
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

    /* 边缘向内滑动：返回 */
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

    /* 横滑：左滑下一页 / 右滑上一页 */
    if (adx > READER_SWIPE_PX && adx > ady * READER_SWIPE_MIN_RATIO) {
        if (dx < 0) {
            reader_next_page();
        } else {
            reader_prev_page();
        }
        return;
    }

    /* 点击分区：左 1/3 上一页，右 1/3 下一页，中 1/3 展开底边栏（判定用按下
     * 起点，释放帧事件不带坐标；详见 input_touch_event_cb）。 */
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
