/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "espaperplay_config.h"
#include "espaperplay_fonts.h"
#include "espaperplay_input.h"
#include "espaperplay_reader.h"
#include "espaperplay_reader_cover.h"
#include "espaperplay_reader_epub.h"
#include "espaperplay_reader_history.h"
#include "espaperplay_storage.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 阅读器主页：最近阅读（历史）+ SD 卡图书（默认目录递归扫描）
 * ====================================================================
 *
 * 布局：统一状态栏（"阅读器"）+ 两个选项卡（最近阅读 / SD 卡图书）+
 * 封面网格卡片 + 页面指示点。条目以网格块呈现：EPUB 显示封面缩略图
 * （后台探测/解码 + SD 缓存，见 reader 组件 cover 服务），TXT 与无封面
 * 书显示占位框；块下方为文件名（历史块另加阅读进度）。
 *   - 历史：SD 卡持久化的阅读记录（最多 ESPAPERPLAY_READER_HISTORY_MAX 条，
 *     最近优先），点击恢复进度，长按删除单条记录；
 *   - 图书：首次进入「SD 卡图书」选项卡时，在 LVGL 线程同步递归扫描
 *     ESPAPERPLAY_READER_SD_DIR（默认 /sdcard/books）下的 TXT / EPUB 文件（深度与
 *     条数有上限），点击打开阅读。readdir 为只读 SD 访问（不触发 flash 缓存
 *     禁用），与文件页同款做法，无需后台任务，避免任务生命周期竞态。
 * 手势：边缘向内滑动返回；点击块打开；长按历史块删除记录；单键返回。
 */

#define RDH_EDGE_PX 24
#define RDH_EDGE_SWIPE_PX 70
#define RDH_CLICK_MAX_PX 15
#define RDH_SWIPE_MIN_RATIO 1.2f
#define RDH_SWIPE_PX 90 /* 分页切换位移阈值 */
#define RDH_MARGIN 16
#define RDH_BAR_H 30
#define RDH_TAB_H 40
#define RDH_LONG_PRESS_MS 600
#define RDH_MODAL_GUARD_MS 300
#define RDH_MODAL_RELEASE_GRACE_MS 150

#define RDH_BOOKS_MAX 200 /* 递归扫描条目上限 */
#define RDH_DEPTH_MAX 6   /* 递归深度上限 */
#define RDH_PAGE_MAX 32   /* 分页上限（防御） */
#define RDH_TILES_MAX 16  /* 单页网格块上限 */
#define RDH_TILE_W 104    /* 块尺寸目标（随屏高缩放，含下限） */
#define RDH_TILE_H 158
#define RDH_TILE_MIN_W 82
#define RDH_TILE_MIN_H 118
#define RDH_PATH_MAX 256 /* 路径缓冲 */

#define RDH_FONT_NAME (espaperplay_system_get_config()->selected_font)

/** 图书条目（绝对路径；显示时取相对默认目录部分）。 */
typedef struct {
    char path[RDH_PATH_MAX];
} rdh_book_t;

/** 网格块（封面框 + 文件名 [+ 进度]）。 */
typedef struct {
    lv_obj_t *tile;     /*!< 块容器 */
    lv_obj_t *frame;    /*!< 封面框（未就绪时即占位边框；TXT 占位宿主） */
    lv_obj_t *img;      /*!< 封面图片（EPUB 专有） */
    lv_obj_t *name;     /*!< 文件名 */
    lv_obj_t *sub;      /*!< 进度（历史块） */
    char path[RDH_PATH_MAX]; /*!< 该块书籍路径（匹配后台结果） */
    int idx;            /*!< 全局条目序号（-1=空槽） */
    uint8_t *cover_buf; /*!< 已载入封面像素（PSRAM，随块销毁释放） */
    lv_image_dsc_t dsc; /*!< 封面描述符（数据指向 cover_buf） */
} rdh_tile_t;

/* ---- 页面状态 ---- */
static espaperplay_ui_status_bar_t *s_bar = NULL;
static lv_obj_t *s_btn_hist = NULL;
static lv_obj_t *s_btn_books = NULL;
static lv_obj_t *s_hint_label = NULL;
static lv_obj_t *s_card = NULL;
static rdh_tile_t s_tiles[RDH_TILES_MAX];
static int s_tile_cnt = 0;
static lv_timer_t *s_cover_timer = NULL; /*!< 封面结果轮询（LVGL 线程） */
static lv_obj_t *s_dots[RDH_PAGE_MAX];

static int s_tab = 0; /* 0=历史 1=图书 */
static int s_page = 0;
static int s_page_count = 1;
static int s_per_page = 8;
static float s_scale = 1.0f;
static int s_card_w = 0;
static int s_card_x = 0;
static int s_card_y = 0;
static int s_card_h = 0;
/* 网格几何（rdh_grid_calc 产出） */
static int s_cols = 1;
static int s_tile_w = RDH_TILE_W;
static int s_tile_h = RDH_TILE_H;
static int s_cover_w = 64; /*!< 封面框宽（封面按 3:4 适配框内） */
static int s_cover_h = 85; /*!< 封面框高 */

static espaperplay_reader_history_entry_t s_hist[ESPAPERPLAY_READER_HISTORY_MAX];
static int s_hist_cnt = 0;
static rdh_book_t *s_books = NULL;
static int s_book_cnt = 0;
static bool s_books_scanned = false; /*!< 本次进入页面后是否已扫描过图书（避免重复扫描） */

static bool s_active = false;
static bool s_touch_down = false;
static bool s_await_release = false;
static uint32_t s_touch_down_tick = 0;
static int s_touch_hit = -1;
static lv_point_t s_touch_start = {0, 0};
static lv_point_t s_touch_last = {0, 0};

/* 模态（长按删除历史二次确认） */
static lv_obj_t *s_modal = NULL;
static uint32_t s_modal_guard_until = 0;
static bool s_modal_track_release = false;
static int s_pending_hist_idx = -1;

/* 前向声明 */
static void rdh_rebuild(void);
static void rdh_confirm_open(const char *title, const char *msg, bool alert_only);
static void rdh_modal_close(void);

/* ------------------------------------------------------------------ */
/* 工具                                                                 */
/* ------------------------------------------------------------------ */

static lv_font_t *rdh_font(int size_px) {
    const char *name = RDH_FONT_NAME[0] ? RDH_FONT_NAME : ESPAPERPLAY_FONTS_DEFAULT_NAME;
    return espaperplay_fonts_load(name, (uint32_t)size_px, ESPAPERPLAY_FONT_STYLE_NORMAL);
}

static lv_obj_t *rdh_label_create(lv_obj_t *parent, const char *text, int font_px,
                                  lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_font_t *font = rdh_font(font_px);
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

static void rdh_screen_size(int32_t *out_w, int32_t *out_h) {
    lv_display_t *disp = lv_display_get_default();
    *out_w = lv_display_get_horizontal_resolution(disp);
    *out_h = lv_display_get_vertical_resolution(disp);
}

static int rdh_scaled(int v) { return (int)(v * s_scale); }

/** 路径拼接并检测截断。 */
static bool rdh_join(char *dst, size_t n, const char *a, const char *b) {
    const size_t need = strlen(a) + 1 + strlen(b) + 1;
    if (need > n) {
        return false;
    }
    strlcpy(dst, a, n);
    strlcat(dst, "/", n);
    strlcat(dst, b, n);
    return true;
}

/** 取路径最后一段（文件名）。 */
static const char *rdh_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
}

/** 取相对默认图书目录的路径（用于显示；目录缺失时返回 basename）。 */
static const char *rdh_rel(const char *full) {
    const size_t base_len = strlen(ESPAPERPLAY_READER_SD_DIR);
    if (strncmp(full, ESPAPERPLAY_READER_SD_DIR, base_len) == 0 && full[base_len] == '/') {
        return full + base_len + 1;
    }
    return rdh_basename(full);
}

/** 书名显示截断（UTF-8 边界 + 省略号）。 */
static void rdh_disp(const char *src, char *dst, size_t n) {
    size_t len = strlen(src);
    bool truncated = false;
    if (len > n - 4) {
        len = n - 4;
        while (len > 0 && (((unsigned char)src[len] & 0xC0) == 0x80)) {
            len--;
        }
        truncated = true;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    if (truncated) {
        strlcat(dst, "…", n);
    }
}

/* ------------------------------------------------------------------ */
/* 递归扫描（worker，内部 RAM 栈）                                       */
/* ------------------------------------------------------------------ */

/** 支持的图书格式（TXT / EPUB）。 */
static bool rdh_is_book(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot != NULL && (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".epub") == 0);
}

static void rdh_scan_dir(const char *dir, int depth, int *cnt) {
    if (depth > RDH_DEPTH_MAX || *cnt >= RDH_BOOKS_MAX || s_books == NULL) {
        return;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *cnt < RDH_BOOKS_MAX) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char child[RDH_PATH_MAX];
        if (!rdh_join(child, sizeof(child), dir, e->d_name)) {
            continue;
        }
        if (e->d_type == DT_DIR) {
            rdh_scan_dir(child, depth + 1, cnt);
        } else if (rdh_is_book(e->d_name)) {
            strlcpy(s_books[*cnt].path, child, sizeof(s_books[*cnt].path));
            (*cnt)++;
        }
    }
    closedir(d);
}

static int rdh_book_cmp(const void *a, const void *b) {
    const rdh_book_t *x = a;
    const rdh_book_t *y = b;
    return strcmp(x->path, y->path);
}

/**
 * 同步递归扫描默认图书目录（LVGL 线程内调用）。
 *
 * readdir 为只读 SD 访问（不触发 flash 缓存禁用），在 LVGL 线程执行与文件页
 * 同款做法，安全且省去后台任务（内部 RAM 栈/任务生命周期竞态/重复创建失败）。
 * 扫描有深度/条数上限，耗时约几十毫秒，可忽略。
 */
static void rdh_scan_sync(void) {
    if (s_books == NULL || !espaperplay_storage_is_mounted()) {
        return;
    }
    int cnt = 0;
    rdh_scan_dir(ESPAPERPLAY_READER_SD_DIR, 0, &cnt);
    qsort(s_books, cnt, sizeof(rdh_book_t), rdh_book_cmp);
    s_book_cnt = cnt;
    s_books_scanned = true;
    ESP_LOGI(TAG, "rdh: scanned %s -> %d book(s)", ESPAPERPLAY_READER_SD_DIR, cnt);
}

/* ------------------------------------------------------------------ */
/* 列表构建                                                             */
/* ------------------------------------------------------------------ */

/** 当前选项卡条目数。 */
static int rdh_list_count(void) { return s_tab == 0 ? s_hist_cnt : s_book_cnt; }

/** 销毁网格块与指示点（含封面缓冲释放与在途封面请求作废）。 */
static void rdh_list_destroy(void) {
    espaperplay_reader_cover_cancel();
    for (int i = 0; i < RDH_TILES_MAX; i++) {
        if (s_tiles[i].tile != NULL) {
            lv_obj_del(s_tiles[i].tile);
        }
        if (s_tiles[i].cover_buf != NULL) {
            heap_caps_free(s_tiles[i].cover_buf);
        }
        memset(&s_tiles[i], 0, sizeof(rdh_tile_t));
        s_tiles[i].idx = -1;
    }
    s_tile_cnt = 0;
    for (int i = 0; i < RDH_PAGE_MAX; i++) {
        if (s_dots[i] != NULL) {
            lv_obj_del(s_dots[i]);
            s_dots[i] = NULL;
        }
    }
}

/** 计算网格几何（列数 / 块尺寸 / 封面框）。 */
static void rdh_grid_calc(void) {
    const int pad = rdh_scaled(10);
    const int gap = rdh_scaled(8);
    int tw = rdh_scaled(RDH_TILE_W);
    if (tw < RDH_TILE_MIN_W) {
        tw = RDH_TILE_MIN_W;
    }
    int th = rdh_scaled(RDH_TILE_H);
    if (th < RDH_TILE_MIN_H) {
        th = RDH_TILE_MIN_H;
    }
    s_cols = (s_card_w - 2 * pad + gap) / (tw + gap);
    if (s_cols < 1) {
        s_cols = 1;
    }
    if (s_cols > RDH_TILES_MAX) {
        s_cols = RDH_TILES_MAX;
    }
    s_tile_w = (s_card_w - 2 * pad - (s_cols - 1) * gap) / s_cols;
    int rows = (s_card_h - 2 * pad + gap) / (th + gap);
    if (rows < 1) {
        rows = 1;
    }
    if (rows > RDH_TILES_MAX / s_cols) {
        rows = RDH_TILES_MAX / s_cols;
    }
    s_tile_h = th;
    s_per_page = s_cols * rows;

    /* 封面框：3:4 适配块内容区（去除文件名 / 进度行） */
    const int name_h = rdh_scaled(22);
    const int sub_h = rdh_scaled(16);
    int cw = s_tile_w - 2 * rdh_scaled(6);
    int ch = s_tile_h - name_h - sub_h - rdh_scaled(10);
    if (ch < 40) {
        ch = 40;
    }
    if (cw > ch * 3 / 4) {
        cw = ch * 3 / 4;
    } else {
        ch = cw * 4 / 3;
    }
    s_cover_w = cw > 24 ? cw : 24;
    s_cover_h = ch > 32 ? ch : 32;
}

/** 构建当前页网格（含历史/图书内容 + 封面请求）。 */
static void rdh_build_grid(void) {
    rdh_list_destroy();
    const int total = rdh_list_count();
    const int start = s_page * s_per_page;
    int n = total - start;
    if (n > s_per_page) {
        n = s_per_page;
    }
    if (n > RDH_TILES_MAX) {
        n = RDH_TILES_MAX;
    }
    const int pad = rdh_scaled(10);
    const int gap = rdh_scaled(8);

    for (int i = 0; i < n; i++) {
        const int idx = start + i;
        rdh_tile_t *t = &s_tiles[i];
        memset(t, 0, sizeof(*t));
        t->idx = idx;
        t->path[0] = '\0';
        const char *path = (s_tab == 0) ? s_hist[idx].path : s_books[idx].path;
        strlcpy(t->path, path, sizeof(t->path));

        /* 块容器 */
        const int col = i % s_cols;
        const int row = i / s_cols;
        lv_obj_t *tile = lv_obj_create(s_card);
        t->tile = tile;
        lv_obj_set_size(tile, s_tile_w, s_tile_h);
        lv_obj_set_pos(tile, pad + col * (s_tile_w + gap), pad + row * (s_tile_h + gap));
        lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_radius(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        /* 封面框（未就绪时即占位边框） */
        lv_obj_t *frame = lv_obj_create(tile);
        t->frame = frame;
        lv_obj_set_size(frame, s_cover_w, s_cover_h);
        lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
        lv_obj_set_style_border_color(frame, lv_color_black(), 0);
        lv_obj_set_style_border_width(frame, 1, 0);
        lv_obj_set_style_radius(frame, 0, 0);
        lv_obj_set_style_pad_all(frame, 0, 0);
        lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

        /* 文件名 + 进度 */
        char title[RDH_PATH_MAX];
        char sub[64];
        if (s_tab == 0) {
            /* 历史：书名 + 进度（page 为打包位置：章 << 20 | 章内页） */
            rdh_disp(rdh_basename(t->path), title, sizeof(title));
            const unsigned hch = (unsigned)(s_hist[idx].page >> 20);
            const unsigned hlp = (unsigned)(s_hist[idx].page & 0xFFFFF);
            /* 新格式（EPUB）：total = bit31 | 章内总页数 → 「第 N 章 x/y 页」
             *（x/y 同为章内页号，自洽）；旧格式无章内总数，只显示页号 */
            const bool has_ch_total =
                (s_hist[idx].total & 0x80000000u) != 0 && hch > 0;
            const unsigned hlt = s_hist[idx].total & 0x7FFFFFFFu;
            if (has_ch_total && hlt > 0) {
                snprintf(sub, sizeof(sub), "第%u章 %u/%u页", hch + 1, hlp + 1, hlt);
            } else if (hch > 0) {
                snprintf(sub, sizeof(sub), "第%u章 第%u页", hch + 1, hlp + 1);
            } else if (s_hist[idx].total > 0) {
                snprintf(sub, sizeof(sub), "第 %u / %u 页", hlp + 1, (unsigned)s_hist[idx].total);
            } else {
                snprintf(sub, sizeof(sub), "第 %u 页", hlp + 1);
            }
        } else {
            /* 图书：相对路径 */
            rdh_disp(rdh_rel(t->path), title, sizeof(title));
            sub[0] = '\0';
        }

        lv_obj_t *nl = rdh_label_create(tile, title, 16, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(nl, s_tile_w);
        lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
        lv_obj_align_to(nl, frame, LV_ALIGN_OUT_BOTTOM_MID, 0, rdh_scaled(2));
        t->name = nl;
        if (sub[0] != '\0') {
            lv_obj_t *sl = rdh_label_create(tile, sub, 14, LV_TEXT_ALIGN_CENTER);
            lv_obj_set_width(sl, s_tile_w);
            lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
            lv_obj_align_to(sl, nl, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
            t->sub = sl;
        }

        /* 封面请求 / TXT 占位 */
        if (espaperplay_reader_is_epub(t->path)) {
            t->img = lv_image_create(frame);
            lv_obj_center(t->img); /* 解码尺寸已适配封面框，居中即可 */
            (void)espaperplay_reader_cover_request(t->path, s_cover_w, s_cover_h);
        } else {
            lv_obj_t *tx = rdh_label_create(frame, "TXT", 16, LV_TEXT_ALIGN_CENTER);
            lv_obj_center(tx);
        }
    }
    s_tile_cnt = n;
}

/** 切换分页。 */
static void rdh_show_page(int idx) {
    if (idx < 0 || idx >= s_page_count || idx == s_page) {
        return;
    }
    s_page = idx;
    rdh_build_grid();
    for (int i = 0; i < s_page_count; i++) {
        lv_obj_set_style_bg_color(s_dots[i], i == s_page ? lv_color_black() : lv_color_white(), 0);
    }
}

/** 重建整个列表（条目数/分页/提示），LVGL 线程内调用。 */
static void rdh_rebuild(void) {
    const int total = rdh_list_count();

    if (s_tab == 0) {
        if (s_hist_cnt == 0) {
            lv_label_set_text(s_hint_label, "暂无阅读历史\n\n从「SD 卡图书」打开一本书吧");
            lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (!espaperplay_storage_is_mounted()) {
            lv_label_set_text(s_hint_label, "SD 卡未挂载");
            lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        } else if (s_book_cnt == 0) {
            char hint[128];
            snprintf(hint, sizeof(hint), "目录无图书（TXT / EPUB）\n\n请把书放入 %s",
                     ESPAPERPLAY_READER_SD_DIR);
            lv_label_set_text(s_hint_label, hint);
            lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_move_foreground(s_hint_label);

    rdh_grid_calc();
    s_page_count = (total + s_per_page - 1) / s_per_page;
    if (s_page_count < 1) {
        s_page_count = 1;
    }
    if (s_page >= s_page_count) {
        s_page = s_page_count - 1;
    }

    /* 指示点 */
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    rdh_screen_size(&scr_w, &scr_h);
    const int dots_y = s_card_y + s_card_h + 14;
    for (int i = 0; i < s_page_count && i < RDH_PAGE_MAX; i++) {
        s_dots[i] = lv_obj_create(lv_screen_active());
        lv_obj_set_size(s_dots[i], 10, 10);
        lv_obj_set_pos(s_dots[i], scr_w / 2 + (i - (s_page_count - 1) / 2) * 24 - 5, dots_y);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_dots[i], 1, 0);
        lv_obj_set_style_border_color(s_dots[i], lv_color_black(), 0);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(s_dots[i], i == s_page ? lv_color_black() : lv_color_white(), 0);
    }

    rdh_build_grid();
    espaperplay_ui_status_bar_refresh(s_bar);
    ESP_LOGI(TAG, "rdh: rebuild tab %d -> %d entries, %d page(s), grid %dx%d (cover %dx%d)",
             s_tab, total, s_page_count, s_cols, s_per_page / s_cols, s_cover_w, s_cover_h);
}

/** 切换选项卡。 */
static void rdh_set_tab(int tab) {
    if (tab < 0 || tab > 1 || tab == s_tab) {
        return;
    }
    s_tab = tab;
    s_page = 0;
    const bool hist_active = (tab == 0);
    lv_obj_set_style_bg_color(s_btn_hist, hist_active ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_btn_hist, 0),
                                hist_active ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_btn_books, hist_active ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_btn_books, 0),
                                hist_active ? lv_color_black() : lv_color_white(), 0);
    if (tab == 1 && !s_books_scanned) {
        rdh_scan_sync(); /* 图书列表：首次进入时同步递归扫描（readdir 只读，LVGL 线程安全） */
    }
    rdh_rebuild();
}

/* ------------------------------------------------------------------ */
/* 打开 / 模态                                                           */
/* ------------------------------------------------------------------ */

/** 打开一本书（reader_open 成功则进入阅读视图）。 */
static void rdh_open(const char *full) {
    const esp_err_t err = espaperplay_reader_open(full);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "rdh: open %s", full);
        (void)espaperplay_ui_page_push_lv(&espaperplay_ui_page_reader_view);
    } else if (err == ESP_ERR_NO_MEM) {
        rdh_confirm_open("提示", "文件过大，无法打开", true);
    } else {
        rdh_confirm_open("提示", "打开失败", true);
    }
}

static bool rdh_modal_click_ok(void) {
    return s_modal != NULL && lv_tick_get() >= s_modal_guard_until;
}

static void rdh_modal_overlay_cb(lv_event_t *e) {
    (void)e;
    if (!rdh_modal_click_ok()) {
        return;
    }
    rdh_modal_close();
}

static void rdh_modal_close(void) {
    if (s_modal != NULL) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }
    s_modal_guard_until = 0;
    s_modal_track_release = false;
    s_pending_hist_idx = -1;
}

/** 删除历史 确定：异步删除后重载重建。 */
static void rdh_del_ok_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (!rdh_modal_click_ok()) {
        return;
    }
    const int idx = s_pending_hist_idx;
    if (idx >= 0 && idx < s_hist_cnt) {
        (void)espaperplay_reader_history_remove(s_hist[idx].path);
    }
    rdh_modal_close();
    (void)espaperplay_reader_history_flush(1000);
    (void)espaperplay_reader_history_load(s_hist, ESPAPERPLAY_READER_HISTORY_MAX, &s_hist_cnt);
    rdh_rebuild();
}

/** 删除历史 取消。 */
static void rdh_del_cancel_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (!rdh_modal_click_ok()) {
        return;
    }
    rdh_modal_close();
}

/** 打开确认/提示模态（alert_only=true 仅「确定」）。 */
static void rdh_confirm_open(const char *title, const char *msg, bool alert_only) {
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    rdh_screen_size(&scr_w, &scr_h);

    /* 全屏覆盖层：背景透明，避免 BW 模式下浅灰渲染成纯白盖掉列表。 */
    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, scr_w, scr_h);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal, rdh_modal_overlay_cb, LV_EVENT_CLICKED, NULL);
    s_modal_guard_until = 0;
    s_modal_track_release = false;

    const int card_w = scr_w - 2 * RDH_MARGIN;
    const int card_h = rdh_scaled(250) < 210 ? 210 : rdh_scaled(250);
    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = rdh_label_create(card, title, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(tl, LV_PCT(100));
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(tl, 0, rdh_scaled(14));

    if (msg != NULL) {
        lv_obj_t *ml = rdh_label_create(card, msg, 16, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(ml, card_w - rdh_scaled(40));
        lv_obj_set_pos(ml, rdh_scaled(20), rdh_scaled(52));
        lv_label_set_long_mode(ml, LV_LABEL_LONG_WRAP);
    }

    const int bh = rdh_scaled(44) < 38 ? 38 : rdh_scaled(44);
    const int by = card_h - bh - rdh_scaled(14);
    lv_obj_t *btn;
    if (alert_only) {
        btn = lv_button_create(card);
        lv_obj_set_size(btn, card_w - rdh_scaled(48), bh);
        lv_obj_set_pos(btn, rdh_scaled(24), by);
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, "确定");
        lv_obj_set_style_text_color(bl, lv_color_white(), 0);
        if (rdh_font(20) != NULL) {
            lv_obj_set_style_text_font(bl, rdh_font(20), 0);
        }
        lv_obj_center(bl);
        lv_obj_add_event_cb(btn, rdh_del_cancel_cb, LV_EVENT_CLICKED, NULL);
    } else {
        const int bw = (card_w - rdh_scaled(60)) / 2;
        btn = lv_button_create(card);
        lv_obj_set_size(btn, bw, bh);
        lv_obj_set_pos(btn, rdh_scaled(24), by);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, "取消");
        lv_obj_set_style_text_color(bl, lv_color_black(), 0);
        if (rdh_font(20) != NULL) {
            lv_obj_set_style_text_font(bl, rdh_font(20), 0);
        }
        lv_obj_center(bl);
        lv_obj_add_event_cb(btn, rdh_del_cancel_cb, LV_EVENT_CLICKED, NULL);

        btn = lv_button_create(card);
        lv_obj_set_size(btn, bw, bh);
        lv_obj_set_pos(btn, rdh_scaled(36) + bw, by);
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        bl = lv_label_create(btn);
        lv_label_set_text(bl, "确定");
        lv_obj_set_style_text_color(bl, lv_color_white(), 0);
        if (rdh_font(20) != NULL) {
            lv_obj_set_style_text_font(bl, rdh_font(20), 0);
        }
        lv_obj_center(bl);
        lv_obj_add_event_cb(btn, rdh_del_ok_cb, LV_EVENT_CLICKED, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* 命中检测                                                             */
/* ------------------------------------------------------------------ */

/** 块屏幕坐标（累加父偏移）。 */
static int rdh_tile_screen_x(const lv_obj_t *obj) {
    int x = 0;
    const lv_obj_t *p = obj;
    while (p != NULL && lv_obj_get_parent(p) != NULL) {
        x += lv_obj_get_x(p);
        p = lv_obj_get_parent(p);
    }
    return x;
}

static int rdh_tile_screen_y(const lv_obj_t *obj) {
    int y = 0;
    const lv_obj_t *p = obj;
    while (p != NULL && lv_obj_get_parent(p) != NULL) {
        y += lv_obj_get_y(p);
        p = lv_obj_get_parent(p);
    }
    return y;
}

static int rdh_hit_tile(const lv_point_t *p) {
    for (int i = 0; i < s_tile_cnt; i++) {
        const lv_obj_t *tile = s_tiles[i].tile;
        if (tile == NULL) {
            continue;
        }
        const int x = rdh_tile_screen_x(tile);
        const int y = rdh_tile_screen_y(tile);
        if (p->x >= x && p->x < x + lv_obj_get_width(tile) && p->y >= y &&
            p->y < y + lv_obj_get_height(tile)) {
            return s_tiles[i].idx;
        }
    }
    return -1;
}

/** 长按删除历史（二次确认）。 */
static void rdh_delete_history(int idx) {
    if (idx < 0 || idx >= s_hist_cnt) {
        return;
    }
    char disp[64];
    rdh_disp(rdh_basename(s_hist[idx].path), disp, sizeof(disp));
    char msg[128];
    snprintf(msg, sizeof(msg), "将删除「%s」的阅读记录。", disp);
    s_pending_hist_idx = idx;
    rdh_confirm_open("删除记录", msg, false);
    /* 长按期间打开模态：启用点击抑制（与文件页同款） */
    s_modal_guard_until = lv_tick_get() + RDH_MODAL_GUARD_MS;
    s_modal_track_release = true;
}

/* ------------------------------------------------------------------ */
/* 页面生命周期                                                          */
/* ------------------------------------------------------------------ */

/** 选项卡按钮回调：切换 最近阅读(0) / SD 卡图书(1)。 */
static void rdh_tab_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    rdh_set_tab((int)(intptr_t)lv_event_get_user_data(e));
}

/** 封面结果轮询（LVGL 线程）：就绪缩略图 → 匹配的可见块；失配（已翻页）丢弃。 */
static void rdh_cover_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_active) {
        return;
    }
    for (int k = 0; k < 4; k++) { /* 单轮上限，避免长时间占用 LVGL */
        espaperplay_reader_cover_result_t res;
        if (!espaperplay_reader_cover_poll(&res)) {
            break;
        }
        rdh_tile_t *tile = NULL;
        for (int i = 0; i < s_tile_cnt; i++) {
            if (strcmp(s_tiles[i].path, res.path) == 0) {
                tile = &s_tiles[i];
                break;
            }
        }
        if (tile == NULL) { /* 翻页 / 切 tab 后的迟到结果 */
            if (res.buf != NULL) {
                heap_caps_free(res.buf);
            }
            continue;
        }
        if (res.buf == NULL || tile->img == NULL) {
            continue; /* 无封面：占位框保留原样 */
        }
        if (tile->cover_buf != NULL) {
            heap_caps_free(tile->cover_buf);
        }
        tile->cover_buf = res.buf;
        memset(&tile->dsc, 0, sizeof(tile->dsc));
        tile->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        tile->dsc.header.w = res.w;
        tile->dsc.header.h = res.h;
        tile->dsc.header.stride = res.w * 2;
        tile->dsc.data_size = (uint32_t)res.w * res.h * 2;
        tile->dsc.data = res.buf;
        lv_image_set_src(tile->img, &tile->dsc);
    }
}

/** 阅读器主页构建（页面 enter）。 */
static void rdh_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t scr_w = 0;
    int32_t scr_h = 0;
    rdh_screen_size(&scr_w, &scr_h);

    s_scale = (float)scr_h / 800.0f;
    s_bar = espaperplay_ui_status_bar_create(scr, RDH_BAR_H, "阅读器", false);
    espaperplay_ui_status_bar_refresh(s_bar);

    /* 选项卡 */
    const int tab_w = (scr_w - 3 * RDH_MARGIN) / 2;
    s_btn_hist = lv_button_create(scr);
    lv_obj_set_size(s_btn_hist, tab_w, RDH_TAB_H);
    lv_obj_set_pos(s_btn_hist, RDH_MARGIN, RDH_BAR_H + 6);
    lv_obj_set_style_bg_color(s_btn_hist, lv_color_black(), 0);
    lv_obj_set_style_radius(s_btn_hist, 8, 0);
    lv_obj_t *hl = lv_label_create(s_btn_hist);
    lv_label_set_text(hl, "最近阅读");
    lv_obj_set_style_text_color(hl, lv_color_white(), 0);
    if (rdh_font(20) != NULL) {
        lv_obj_set_style_text_font(hl, rdh_font(20), 0);
    }
    lv_obj_center(hl);
    lv_obj_add_event_cb(s_btn_hist, rdh_tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);

    s_btn_books = lv_button_create(scr);
    lv_obj_set_size(s_btn_books, tab_w, RDH_TAB_H);
    lv_obj_set_pos(s_btn_books, RDH_MARGIN + tab_w + RDH_MARGIN, RDH_BAR_H + 6);
    lv_obj_set_style_bg_color(s_btn_books, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_btn_books, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_btn_books, 2, 0);
    lv_obj_set_style_radius(s_btn_books, 8, 0);
    lv_obj_t *bl = lv_label_create(s_btn_books);
    lv_label_set_text(bl, "SD 卡图书");
    lv_obj_set_style_text_color(bl, lv_color_black(), 0);
    if (rdh_font(20) != NULL) {
        lv_obj_set_style_text_font(bl, rdh_font(20), 0);
    }
    lv_obj_center(bl);
    lv_obj_add_event_cb(s_btn_books, rdh_tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

    /* 列表卡片 */
    s_card_x = RDH_MARGIN;
    s_card_y = RDH_BAR_H + 6 + RDH_TAB_H + 6;
    s_card_w = scr_w - 2 * RDH_MARGIN;
    s_card_h = scr_h - s_card_y - 34;
    if (s_card_h < 100) {
        s_card_h = 100;
    }

    s_card = lv_obj_create(scr);
    lv_obj_set_size(s_card, s_card_w, s_card_h);
    lv_obj_set_pos(s_card, s_card_x, s_card_y);
    lv_obj_set_style_bg_color(s_card, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_card, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_radius(s_card, 12, 0);
    lv_obj_set_style_pad_all(s_card, 0, 0);
    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    /* 提示 */
    s_hint_label = rdh_label_create(scr, "", 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_hint_label, s_card_w);
    lv_obj_set_pos(s_hint_label, s_card_x, s_card_y + s_card_h / 2 - 20);
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);

    /* 图书缓冲（PSRAM；图书页首次进入时同步填充） */
    if (s_books == NULL) {
        s_books = heap_caps_malloc((size_t)RDH_BOOKS_MAX * sizeof(rdh_book_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_books == NULL) {
        ESP_LOGE(TAG, "rdh: books alloc failed");
    }

    /* 封面结果轮询定时器（LVGL 线程，退出时删除） */
    if (s_cover_timer == NULL) {
        s_cover_timer = lv_timer_create(rdh_cover_timer_cb, 250, NULL);
    }

    /* 状态复位 */
    s_active = true;
    s_tab = 0;
    s_page = 0;
    s_books_scanned = false; /* 进入页面需重新扫描图书目录 */
    s_touch_down = false;
    s_await_release = false;
    s_touch_hit = -1;
    s_modal = NULL;
    s_modal_guard_until = 0;
    s_modal_track_release = false;
    s_pending_hist_idx = -1;

    /* 历史：等待阅读视图退出时投递的写操作落盘后重载 */
    (void)espaperplay_reader_history_flush(1500);
    (void)espaperplay_reader_history_load(s_hist, ESPAPERPLAY_READER_HISTORY_MAX, &s_hist_cnt);

    rdh_rebuild();

    ESP_LOGI(TAG, "reader home entered (hist=%d)", s_hist_cnt);
}

/** 阅读器主页退出。 */
static void rdh_exit(void) {
    s_active = false;
    if (s_cover_timer != NULL) {
        lv_timer_del(s_cover_timer);
        s_cover_timer = NULL;
    }
    rdh_modal_close();
    rdh_list_destroy();
    if (s_books != NULL) {
        heap_caps_free(s_books);
        s_books = NULL;
    }
    s_book_cnt = 0;
    s_books_scanned = false;
    s_bar = NULL;
    s_btn_hist = NULL;
    s_btn_books = NULL;
    s_hint_label = NULL;
    s_card = NULL;
    ESP_LOGI(TAG, "reader home exited");
}

/* ------------------------------------------------------------------ */
/* 按键 / 触摸                                                          */
/* ------------------------------------------------------------------ */

/** 按键：模态打开时单击关闭；否则返回上一页。 */
static void rdh_on_key(const espaperplay_input_event_t *event) {
    if (event->key_action != ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK) {
        return;
    }
    if (s_modal != NULL) {
        rdh_modal_close();
        return;
    }
    if (espaperplay_ui_page_depth() > 1) {
        ESP_LOGI(TAG, "rdh: single click -> pop back");
        espaperplay_ui_page_pop_lv();
    }
}

/** 触摸：长按历史块删除；点击块打开；边缘滑动返回。 */
static void rdh_on_touch(const espaperplay_input_event_t *event) {
    lv_point_t p;
    espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

    /* 长按打开模态：观察其后的首次物理释放并延长抑制窗口 */
    if (!event->touch_pressed && s_modal_track_release) {
        s_modal_track_release = false;
        const uint32_t until = lv_tick_get() + RDH_MODAL_RELEASE_GRACE_MS;
        if ((int32_t)(until - s_modal_guard_until) > 0) {
            s_modal_guard_until = until;
        }
    }
    /* 长按后锁存：忽略一切按下帧直到物理释放 */
    if (s_await_release) {
        if (!event->touch_pressed) {
            s_await_release = false;
        }
        return;
    }
    if (s_modal != NULL) {
        return;
    }

    if (event->touch_pressed) {
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start = p;
            s_touch_down_tick = lv_tick_get();
            s_touch_hit = rdh_hit_tile(&p);
        }
        s_touch_last = p;

        /* 长按判定（仅历史块）：按住达阈值且位移极小，弹删除确认 */
        if (s_touch_hit >= 0 && s_tab == 0 &&
            lv_tick_elaps(s_touch_down_tick) >= RDH_LONG_PRESS_MS &&
            abs(p.x - s_touch_start.x) <= RDH_CLICK_MAX_PX &&
            abs(p.y - s_touch_start.y) <= RDH_CLICK_MAX_PX) {
            const int idx = s_touch_hit;
            s_touch_down = false;
            s_touch_hit = -1;
            s_await_release = true;
            ESP_LOGI(TAG, "rdh: long press history %d", idx);
            rdh_delete_history(idx);
        }
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
    const int hit = s_touch_hit;
    s_touch_hit = -1;

    /* 边缘向内滑动返回 */
    if (adx > RDH_EDGE_SWIPE_PX && adx > ady * RDH_SWIPE_MIN_RATIO) {
        int32_t scr_w = 0;
        int32_t scr_h = 0;
        rdh_screen_size(&scr_w, &scr_h);
        if ((s_touch_start.x < RDH_EDGE_PX && dx > 0) ||
            (s_touch_start.x > scr_w - RDH_EDGE_PX && dx < 0)) {
            if (espaperplay_ui_page_depth() > 1) {
                ESP_LOGI(TAG, "rdh: edge swipe -> pop back");
                espaperplay_ui_page_pop_lv();
            }
            return;
        }
    }

    /* 中间横向滑动：切换分页（优先于点击，与文件/设置页一致） */
    if (adx > RDH_SWIPE_PX && adx > ady * RDH_SWIPE_MIN_RATIO) {
        rdh_show_page(s_page + (dx < 0 ? 1 : -1));
        return;
    }

    /* 小位移点击：块 -> 打开；选项卡按钮由 LVGL 控件处理 */
    if (adx <= RDH_CLICK_MAX_PX && ady <= RDH_CLICK_MAX_PX) {
        if (hit >= 0) {
            if (s_tab == 0) {
                ESP_LOGI(TAG, "rdh: open history %s", s_hist[hit].path);
                rdh_open(s_hist[hit].path);
            } else {
                ESP_LOGI(TAG, "rdh: open book %s", s_books[hit].path);
                rdh_open(s_books[hit].path);
            }
        }
    }
}

/** 阅读器主页页面实例。 */
const espaperplay_ui_page_t espaperplay_ui_page_reader = {rdh_enter, rdh_exit, rdh_on_key,
                                                          rdh_on_touch};
