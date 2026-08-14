/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"

#include "espaperplay_gui_lv.h"
#include "espaperplay_ui.h"

#include "demos/lv_demos.h"
#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/** benchmark 屏构建（在 LVGL 线程内执行，经 espaperplay_gui_lv_call 投递）。
 *
 * 直接启动 LVGL 自带 benchmark：连续多场景高速全屏/局部重绘（动画、文字、
 * 图形、图像），制造高频脏区提交与排队合并，充分暴露异步刷新链路
 * （快照合并 + worker 局刷）在高压下的错位/撕裂问题。 */
static void benchmark_build(void *arg) {
    (void)arg;
    lv_demo_benchmark();
    ESP_LOGI(TAG, "LVGL benchmark started");
}

/** 展示 LVGL 自带 benchmark 测试屏。须在 espaperplay_gui_lv_start() 之后调用。 */
void espaperplay_ui_benchmark_show(void) {
    esp_err_t err = espaperplay_gui_lv_call(benchmark_build, NULL, 2000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "benchmark dispatch failed: %s", esp_err_to_name(err));
    }
}
