/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"

#include "espaperplay_gui_lv.h"
#include "espaperplay_ui.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/** 演示屏内容构建（在 LVGL 线程内执行，经 espaperplay_gui_lv_call 投递）。 */
static void demo_screen_build(void *arg) {
    (void)arg;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "ESPaperPlay LVGL ready");
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);

    ESP_LOGI(TAG, "demo screen shown");
}

/** 最小就绪演示屏：白底 + 居中标签。须在 espaperplay_gui_lv_start() 之后调用。 */
void espaperplay_ui_demo_show(void) {
    esp_err_t err = espaperplay_gui_lv_call(demo_screen_build, NULL, 2000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "demo screen dispatch failed: %s", esp_err_to_name(err));
    }
}
