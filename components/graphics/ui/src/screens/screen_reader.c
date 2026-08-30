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
#include "espaperplay_reader.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"

#include "lvgl.h"
#include "lvgl_private.h"

static const char *TAG = "ESPaperPlay_UI";

static espaperplay_ui_status_bar_t *s_bar = NULL; /*!< 统一状态栏 */
static lv_obj_t *s_content_label = NULL;          /*!< 正文标签 */
static lv_obj_t *s_page_label = NULL;             /*!< 页码指示 */
static lv_obj_t *s_hint_label = NULL;             /*!< 提示（无文档/空文件） */

#define READER_FONT_SIZE 20
#define READER_LINE_SPACE 4
#define READER_MARGIN 16
#define READER_STATUS_H 30
#define READER_FOOTER_H 30
#define READER_EDGE_PX 24
#define READER_EDGE_SWIPE_PX 70
#define READER_SWIPE_PX 90
#define READER_CLICK_MAX_PX 15
#define READER_SWIPE_MIN_RATIO 1.2f

#define READER_FONT_NAME                                                                           \
    (espaperplay_system_get_config()                                                               \
         ->selected_font) /* 当前选用字体（SD 优先，缺则回退 Flash 子集） */

/* 分页状态 */
static uint32_t *s_offsets = NULL; /*!< 每页起始字节偏移（大小 s_page_count+1，末项为文本长度） */
static int s_page_count = 0;       /*!< 总页数 */
static int s_cur_page = 0;         /*!< 当前页（0 基） */
static const char *s_text = NULL;  /*!< 归一化文本（reader 组件持有） */
static size_t s_text_len = 0;      /*!< 文本长度 */
static int s_content_w = 0;        /*!< 内容区宽度 */
static int s_content_h = 0;        /*!< 内容区高度 */
static bool s_touch_down = false;
static lv_point_t s_touch_start = {0, 0};
static lv_point_t s_touch_last = {0, 0};

/** FreeType 字体按需加载 */
static lv_font_t *reader_font(void) {
    const char *name = READER_FONT_NAME[0] ? READER_FONT_NAME : ESPAPERPLAY_FONTS_DEFAULT_NAME;
    lv_font_t *f = espaperplay_fonts_load(name, READER_FONT_SIZE, ESPAPERPLAY_FONT_STYLE_NORMAL);
    if (f == NULL) {
        f = espaperplay_fonts_load(ESPAPERPLAY_FONTS_DEFAULT_NAME, READER_FONT_SIZE,
                                   ESPAPERPLAY_FONT_STYLE_NORMAL);
    }
    return f;
}

/** 逻辑分辨率 */
static void reader_screen_size(int32_t *out_w, int32_t *out_h) {
    lv_display_t *disp = lv_display_get_default();
    *out_w = lv_display_get_horizontal_resolution(disp);
    *out_h = lv_display_get_vertical_resolution(disp);
}

/** 释放分页表 */
static void reader_free_pages(void) {
    if (s_offsets != NULL) {
        heap_caps_free(s_offsets);
        s_offsets = NULL;
    }
    s_page_count = 0;
    s_cur_page = 0;
}

/**
 * @brief 按当前字体与内容区尺寸分页。
 *
 * 使用 lv_text_get_next_line 按 max_width 自动换行，显式 \n 也计为一行。
 * 中文需 BREAK_ALL 才能在任意字符间换行。
 */
static void reader_paginate(void) {
    reader_free_pages();
    if (s_text == NULL || s_text_len == 0) {
        return;
    }
    lv_font_t *font = reader_font();
    if (font == NULL) {
        ESP_LOGE(TAG, "reader: font load failed");
        return;
    }
    const int line_height = lv_font_get_line_height(font) + READER_LINE_SPACE;
    if (line_height <= 0 || s_content_h <= 0 || s_content_w <= 0) {
        return;
    }
    int max_lines = s_content_h / line_height;
    if (max_lines < 1) {
        max_lines = 1;
    }

    /* 预分配：按每页约 400 字节估算，512KB 文本约 1280 页，取 2048 起步 */
    int cap = 2048;
    s_offsets =
        heap_caps_malloc((size_t)(cap + 1) * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_offsets == NULL) {
        s_offsets = heap_caps_malloc((size_t)(cap + 1) * sizeof(uint32_t), MALLOC_CAP_8BIT);
    }
    if (s_offsets == NULL) {
        ESP_LOGE(TAG, "reader: offsets alloc failed");
        return;
    }

    lv_text_attributes_t attr;
    lv_text_attributes_init(&attr);
    attr.letter_space = 0;
    attr.line_space = READER_LINE_SPACE;
    attr.max_width = s_content_w;
    attr.text_flags = LV_TEXT_FLAG_BREAK_ALL;

    uint32_t pos = 0;
    int line_cnt = 0;
    int page_cnt = 0;
    s_offsets[0] = 0;
    page_cnt = 1; /* 已有第一页 */

    while (pos < s_text_len) {
        uint32_t remain = (uint32_t)(s_text_len - pos);
        uint32_t adv = lv_text_get_next_line(&s_text[pos], remain, font, NULL, &attr);
        if (adv == 0) {
            break;
        }
        pos += adv;
        line_cnt++;
        if (line_cnt >= max_lines && pos < s_text_len) {
            /* 页满，切页 */
            if (page_cnt >= cap) {
                int new_cap = cap * 2;
                uint32_t *new_off =
                    heap_caps_realloc(s_offsets, (size_t)(new_cap + 1) * sizeof(uint32_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (new_off == NULL) {
                    new_off = heap_caps_realloc(s_offsets, (size_t)(new_cap + 1) * sizeof(uint32_t),
                                                MALLOC_CAP_8BIT);
                }
                if (new_off == NULL) {
                    ESP_LOGE(TAG, "reader: offsets realloc failed (%d pages)", page_cnt);
                    break;
                }
                s_offsets = new_off;
                cap = new_cap;
            }
            s_offsets[page_cnt++] = pos;
            line_cnt = 0;
        }
    }
    /* 末项为文本长度，便于取页区间 */
    if (page_cnt >= cap) {
        uint32_t *new_off = heap_caps_realloc(s_offsets, (size_t)(cap + 2) * sizeof(uint32_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_off != NULL) {
            s_offsets = new_off;
            cap++;
        }
    }
    if (s_offsets != NULL) {
        s_offsets[page_cnt] = (uint32_t)s_text_len;
        /* 若最后一页为空（恰好页满结束），去掉空页 */
        if (page_cnt > 0 && s_offsets[page_cnt] == s_offsets[page_cnt - 1]) {
            page_cnt--;
        }
        if (page_cnt == 0) {
            page_cnt = 1;
            s_offsets[0] = 0;
            s_offsets[1] = (uint32_t)s_text_len;
        }
    }
    s_page_count = page_cnt;
    s_cur_page = 0;
    ESP_LOGI(TAG, "reader: paginated %u bytes -> %d pages (%dx%d, %d lines/page)",
             (unsigned)s_text_len, s_page_count, s_content_w, s_content_h, max_lines);
}

/** 显示指定页（0 基） */
static void reader_show_page(int idx) {
    if (s_content_label == NULL) {
        return;
    }
    if (s_page_count == 0 || s_offsets == NULL || s_text == NULL) {
        lv_label_set_text(s_content_label, "");
        if (s_page_label != NULL) {
            lv_label_set_text(s_page_label, "0 / 0");
        }
        return;
    }
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= s_page_count) {
        idx = s_page_count - 1;
    }
    s_cur_page = idx;
    uint32_t start = s_offsets[idx];
    uint32_t end = s_offsets[idx + 1];
    uint32_t len = end - start;
    char *tmp = heap_caps_malloc((size_t)len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tmp == NULL) {
        tmp = malloc((size_t)len + 1);
    }
    if (tmp == NULL) {
        ESP_LOGE(TAG, "reader: page buf alloc failed");
        return;
    }
    memcpy(tmp, &s_text[start], len);
    tmp[len] = '\0';
    lv_label_set_text(s_content_label, tmp);
    heap_caps_free(tmp);

    if (s_page_label != NULL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", s_cur_page + 1, s_page_count);
        lv_label_set_text(s_page_label, buf);
    }
    ESP_LOGI(TAG, "reader: show page %d/%d", s_cur_page + 1, s_page_count);
}

static void reader_next_page(void) {
    if (s_page_count == 0) {
        return;
    }
    if (s_cur_page + 1 < s_page_count) {
        reader_show_page(s_cur_page + 1);
    } else {
        ESP_LOGI(TAG, "reader: already last page");
    }
}

static void reader_prev_page(void) {
    if (s_page_count == 0) {
        return;
    }
    if (s_cur_page > 0) {
        reader_show_page(s_cur_page - 1);
    } else {
        ESP_LOGI(TAG, "reader: already first page");
    }
}

/** 单次 GRAY4 刷屏：当前帧按四灰阶全屏刷新一次，随后恢复 BW 模式 */
static void reader_do_gray4_once(void) {
    ESP_LOGI(TAG, "reader: single GRAY4 refresh triggered");
    esp_err_t err = espaperplay_gui_show_gray4();
    if (err == ESP_OK) {
        /* 已排队 GRAY4 全刷，立即把模式切回 BW（不额外刷新），下一次翻页仍为 BW 局刷 */
        (void)espaperplay_gui_set_color(ESPAPERPLAY_GUI_COLOR_BW);
        ESP_LOGI(TAG, "reader: GRAY4 queued, mode restored to BW");
    } else {
        ESP_LOGE(TAG, "reader: GRAY4 failed: %s", esp_err_to_name(err));
    }
}

/** 阅读器页构建 */
static void reader_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t scr_w = 0;
    int32_t scr_h = 0;
    reader_screen_size(&scr_w, &scr_h);

    /* 统一状态栏 */
    s_bar = espaperplay_ui_status_bar_create(scr, READER_STATUS_H, "阅读器", false);
    espaperplay_ui_status_bar_refresh(s_bar);

    /* 内容区几何 */
    s_content_w = scr_w - 2 * READER_MARGIN;
    s_content_h = scr_h - READER_STATUS_H - READER_FOOTER_H - 2 * READER_MARGIN;
    if (s_content_h < 100) {
        s_content_h = 100;
    }
    const int content_y = READER_STATUS_H + READER_MARGIN;

    /* 确保回到 BW 模式（阅读器默认 BW 交互，GRAY4 仅单次触发） */
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
    lv_font_t *font = reader_font();
    if (font != NULL) {
        lv_obj_set_style_text_font(s_content_label, font, 0);
    }

    /* 页码指示（底部居中） */
    s_page_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_page_label, lv_color_black(), 0);
    if (font != NULL) {
        lv_obj_set_style_text_font(s_page_label, font, 0);
    }
    lv_obj_set_style_text_align(s_page_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_page_label, LV_PCT(100));
    lv_obj_set_pos(s_page_label, 0, scr_h - READER_FOOTER_H + 6);
    lv_label_set_text(s_page_label, "");

    /* 提示标签（无文档/空文件时居中显示） */
    s_hint_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_hint_label, lv_color_black(), 0);
    if (font != NULL) {
        lv_obj_set_style_text_font(s_hint_label, font, 0);
    }
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_hint_label, LV_PCT(100));
    lv_obj_set_pos(s_hint_label, 0, scr_h / 2 - 20);
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);

    /* 加载文本 */
    s_text = NULL;
    s_text_len = 0;
    if (espaperplay_reader_is_open()) {
        (void)espaperplay_reader_get_text(&s_text, &s_text_len);
    }

    if (s_text == NULL || s_text_len == 0) {
        const char *hint =
            espaperplay_reader_is_open() ? "空文件" : "无文档\n\n请从文件管理打开 TXT";
        lv_label_set_text(s_hint_label, hint);
        lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_content_label, "");
        lv_label_set_text(s_page_label, "0 / 0");
        s_page_count = 0;
        s_cur_page = 0;
        ESP_LOGI(TAG, "reader: no document");
    } else {
        lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        reader_paginate();
        reader_show_page(0);
    }

    s_touch_down = false;
    ESP_LOGI(TAG, "reader screen entered (%u bytes, %d pages)", (unsigned)s_text_len, s_page_count);
}

static void reader_exit(void) {
    reader_free_pages();
    s_bar = NULL;
    s_content_label = NULL;
    s_page_label = NULL;
    s_hint_label = NULL;
    s_text = NULL;
    s_text_len = 0;
    s_touch_down = false;
    ESP_LOGI(TAG, "reader screen exited");
}

/** 按键处理 */
static void reader_on_key(const espaperplay_input_event_t *event) {
    if (event->type != ESPAPERPLAY_INPUT_EVENT_KEY) {
        return;
    }
    switch (event->key_action) {
    case ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK:
        /* 单击：下一页；已是最后一页则返回 */
        if (s_page_count > 0 && s_cur_page + 1 < s_page_count) {
            reader_next_page();
        } else if (s_page_count > 0) {
            ESP_LOGI(TAG, "reader: single click at last page -> pop");
            espaperplay_ui_page_pop_lv();
        } else {
            espaperplay_ui_page_pop_lv();
        }
        break;
    case ESPAPERPLAY_INPUT_KEY_ACTION_DOUBLE_CLICK:
        /* 双击：上一页 */
        if (s_page_count > 0 && s_cur_page > 0) {
            reader_prev_page();
        } else {
            ESP_LOGI(TAG, "reader: double click at first page");
        }
        break;
    case ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_START:
        /* 长按：单次 GRAY4 刷屏（覆盖全局默认的 BW 全刷） */
        reader_do_gray4_once();
        break;
    default:
        break;
    }
}

/** 触摸处理：边缘滑动返回，中间横滑翻页 */
static void reader_on_touch(const espaperplay_input_event_t *event) {
    lv_point_t p;
    espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

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

    /* 边缘向内滑动返回 */
    if (adx > READER_EDGE_SWIPE_PX && adx > ady * READER_SWIPE_MIN_RATIO) {
        int32_t scr_w = 0;
        int32_t scr_h = 0;
        reader_screen_size(&scr_w, &scr_h);
        if ((s_touch_start.x < READER_EDGE_PX && dx > 0) ||
            (s_touch_start.x > scr_w - READER_EDGE_PX && dx < 0)) {
            if (espaperplay_ui_page_depth() > 1) {
                ESP_LOGI(TAG, "reader: edge swipe -> pop");
                espaperplay_ui_page_pop_lv();
            }
            return;
        }
    }
    /* 中间横滑翻页 */
    if (adx > READER_SWIPE_PX && adx > ady * READER_SWIPE_MIN_RATIO) {
        if (dx < 0) {
            reader_next_page();
        } else {
            reader_prev_page();
        }
        return;
    }
    /* 小位移点击：左半屏上一页，右半屏下一页（便于单手操作） */
    if (adx <= READER_CLICK_MAX_PX && ady <= READER_CLICK_MAX_PX) {
        int32_t scr_w = 0;
        int32_t scr_h = 0;
        reader_screen_size(&scr_w, &scr_h);
        if (p.x < scr_w / 2) {
            reader_prev_page();
        } else {
            reader_next_page();
        }
    }
}

/** 阅读器页实例 */
const espaperplay_ui_page_t espaperplay_ui_page_reader = {reader_enter, reader_exit, reader_on_key,
                                                          reader_on_touch};
