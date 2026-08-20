/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "esp_log.h"

#include "espaperplay_fonts.h"
#include "espaperplay_ui.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 阅读器页（占位）
 * ====================================================================
 *
 * 阅读器核心框架尚未实现（见 components/applications/reader），本页仅
 * 提供正式入口：FreeType 中文标题 + "即将推出"提示，单击按键返回主页。
 */

#define READER_FONT_NAME "NotoSansSC_Regular.ttf"

/** 阅读器页构建（页面 enter：屏幕已由页面栈清空）。 */
static void reader_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "阅读器");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_font(title,
                               espaperplay_fonts_load(READER_FONT_NAME, 24,
                                                      ESPAPERPLAY_FONT_STYLE_NORMAL),
                               0);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_pos(title, 0, lv_pct(8));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "即将推出\n\n单击返回");
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_style_text_font(hint,
                               espaperplay_fonts_load(READER_FONT_NAME, 20,
                                                      ESPAPERPLAY_FONT_STYLE_NORMAL),
                               0);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_pos(hint, 0, lv_pct(40));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    ESP_LOGI(TAG, "reader screen entered");
}

/** 阅读器页按键处理：单击返回主页（与天气页一致）。 */
static void reader_on_key(const espaperplay_input_event_t *event) {
    if (event->key_action == ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK) {
        ESP_LOGI(TAG, "reader: single click -> pop back");
        espaperplay_ui_page_pop_lv();
    }
}

/** 阅读器页页面实例。 */
const espaperplay_ui_page_t espaperplay_ui_page_reader = {reader_enter, NULL, reader_on_key,
                                                          NULL};
