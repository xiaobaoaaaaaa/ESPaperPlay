/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "espaperplay_gui_lv.h"
#include "espaperplay_ui.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 局刷压力测试屏
 * ====================================================================
 *
 * 目的：验证 e-paper 异步刷新链路——
 *   1) 多次快速局部刷新不阻塞 LVGL 任务（flush 只快照排队，真实刷新由
 *      worker 异步执行，LVGL 周期应稳定）；
 *   2) 局部刷新只刷脏区（worker 日志的刷新区域尺寸应随内容变化而不同：
 *      小数字 ~100px 宽 vs 大黑块位移时接近全屏）。
 *
 * 测试元素（各自独立区域、不同变化节奏）：
 *   - 计数器：大字号数字，200ms 一跳（小区域快刷）；
 *   - 进度条：200ms 推进（中等区域）；
 *   - 小方块：100x100，每 2s 横移 60px（小-中区域）；
 *   - 状态行：显示 UI 更新计数与实测间隔（dt 漂移 = LVGL 被阻塞）。
 *
 * 元素集中在同一区域（包围盒 < 全屏阈值），验证稳定局部刷新链路；
 * 分散布局会触发"大面积局刷计数"策略（连续 5 次后强制全刷清残影）。
 */

#define TEST_UI_PERIOD_MS 200 /* UI 更新周期（快于 EPD 局部刷新 ~370ms，制造排队合并） */
#define TEST_BLOCK_STEP_MS (TEST_UI_PERIOD_MS * 10) /* 大黑块位移周期：2s */

static lv_obj_t *s_cnt_label = NULL;   /*!< 计数器数字（48 号大字，小区域） */
static lv_obj_t *s_bar = NULL;         /*!< 进度条（中等区域） */
static lv_obj_t *s_block = NULL;       /*!< 大黑块（位移 -> 大面积刷新） */
static lv_obj_t *s_status_label = NULL; /*!< 状态行 */
static uint32_t s_tick = 0;            /*!< UI 更新计数 */
static uint32_t s_last_tick_ms = 0;    /*!< 上次更新时刻（实测周期） */

/** 周期 UI 更新（LVGL 线程内，lv_timer 驱动）。 */
static void test_timer_cb(lv_timer_t *timer) {
    (void)timer;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t dt = now - s_last_tick_ms;
    s_last_tick_ms = now;
    s_tick++;

    /* 1) 计数器：数字宽度随位数变化，invalidate 小区域。 */
    char buf[24];
    snprintf(buf, sizeof(buf), "%u", (unsigned)s_tick);
    lv_label_set_text(s_cnt_label, buf);

    /* 2) 进度条：200ms 推进一格（0..100 循环）。 */
    lv_bar_set_value(s_bar, (int)(s_tick % 101), LV_ANIM_OFF);

    /* 3) 小方块：每 10 tick 横移 60px（20/80/140 往复）。 */
    if (s_tick % 10 == 0) {
        static const int steps[] = {0, 60, 120, 60};
        lv_obj_set_x(s_block, 20 + steps[(s_tick / 10) % 4]);
    }

    /* 4) 状态行：更新计数 + 实测周期（稳定 ≈200ms 说明 LVGL 未被阻塞）。 */
    snprintf(buf, sizeof(buf), "ui %u  dt %ums", (unsigned)s_tick, (unsigned)dt);
    lv_label_set_text(s_status_label, buf);

    if (s_tick % 20 == 0) {
        ESP_LOGI(TAG, "test: tick %u, dt %u ms", (unsigned)s_tick, (unsigned)dt);
    }
}

/** 测试屏构建（LVGL 线程内执行，经 espaperplay_gui_lv_call 投递）。 */
static void test_screen_build(void *arg) {
    (void)arg;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LVGL partial refresh test");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* 测试区集中在左下 ~300x330：包围盒面积 < 全屏阈值（70%），走 PARTIAL。 */
    s_cnt_label = lv_label_create(scr);
    lv_label_set_text(s_cnt_label, "0");
    lv_obj_set_style_text_color(s_cnt_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_cnt_label, &lv_font_montserrat_48, 0);
    lv_obj_align(s_cnt_label, LV_ALIGN_TOP_LEFT, 20, 40);

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 200, 30);
    lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, 20, 150);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    /* 小方块：100x100@(x, 220)，x 在 20..140 间往复位移。 */
    s_block = lv_obj_create(scr);
    lv_obj_set_size(s_block, 100, 100);
    lv_obj_set_pos(s_block, 20, 220);
    lv_obj_set_style_bg_color(s_block, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_block, 0, 0);
    lv_obj_set_style_radius(s_block, 0, 0);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "ui 0  dt 0ms");
    lv_obj_set_style_text_color(s_status_label, lv_color_black(), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 20, 350);

    lv_timer_create(test_timer_cb, TEST_UI_PERIOD_MS, NULL);
    s_last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000);

    ESP_LOGI(TAG, "test screen shown (ui %u ms, block %u ms)", TEST_UI_PERIOD_MS,
             TEST_BLOCK_STEP_MS);
}

/** 展示局刷压力测试屏。须在 espaperplay_gui_lv_start() 之后调用。 */
void espaperplay_ui_test_show(void) {
    esp_err_t err = espaperplay_gui_lv_call(test_screen_build, NULL, 2000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "test screen dispatch failed: %s", esp_err_to_name(err));
    }
}
