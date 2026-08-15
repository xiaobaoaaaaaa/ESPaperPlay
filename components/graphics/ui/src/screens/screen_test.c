/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "espaperplay_input.h"
#include "espaperplay_ui.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 测试页（局刷压力测试 + 按键事件显示）
 * ====================================================================
 *
 * 目的 1：验证 e-paper 异步刷新链路——
 *   1) 多次快速局部刷新不阻塞 LVGL 任务（flush 只快照排队，真实刷新由
 *      worker 异步执行，LVGL 周期应稳定）；
 *   2) 局部刷新只刷脏区（worker 日志的刷新区域尺寸应随内容变化而不同：
 *      小数字 ~100px 宽 vs 大黑块位移时接近全屏）。
 *
 * 目的 2：验证按键链路——实时显示最近一次按键事件（动作 / id / 按压
 *   时长）与累计计数；长按松开返回上一页。
 *
 * 布局全部使用百分比（相对屏幕尺寸），适配不同分辨率 / 比例面板：
 *   左列（压力测试区）：计数器 / 进度条 / 位移方块 / 状态行；
 *   右列（按键测试区）：最近按键事件 / 累计计数。
 *
 * 压力测试元素集中在左侧（包围盒 < 全屏阈值），验证稳定局部刷新链路；
 * 分散布局会触发"大面积局刷计数"策略（连续 5 次后强制全刷清残影）。
 */

#define TEST_UI_PERIOD_MS 200 /* UI 更新周期（快于 EPD 局部刷新 ~370ms，制造排队合并） */
#define TEST_BLOCK_STEP_MS (TEST_UI_PERIOD_MS * 10) /* 大黑块位移周期：2s */

static lv_obj_t *s_cnt_label = NULL;       /*!< 计数器数字（48 号大字，小区域） */
static lv_obj_t *s_bar = NULL;             /*!< 进度条（中等区域） */
static lv_obj_t *s_block = NULL;           /*!< 大黑块（位移 -> 大面积刷新） */
static lv_obj_t *s_status_label = NULL;    /*!< 状态行 */
static lv_obj_t *s_last_key_label = NULL;  /*!< 最近一次按键事件 */
static lv_obj_t *s_key_count_label = NULL; /*!< 按键累计计数 */
static lv_timer_t *s_test_timer = NULL;    /*!< 页面定时器（页面栈退出时删除） */
static uint32_t s_tick = 0;                /*!< UI 更新计数 */
static uint32_t s_last_tick_ms = 0;        /*!< 上次更新时刻（实测周期） */
static uint32_t s_key_count = 0;           /*!< 按键计数（页面实例状态） */

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

    /* 3) 小方块：每 10 tick 横移（8% / 15% 屏宽往复，等价于 60/120px@800px 宽屏）。 */
    if (s_tick % 10 == 0) {
        static const int steps[] = {0, 8, 15, 8};
        lv_obj_set_x(s_block, lv_pct(steps[(s_tick / 10) % 4]));
    }

    /* 4) 状态行：更新计数 + 实测周期（稳定 ≈200ms 说明 LVGL 未被阻塞）。 */
    snprintf(buf, sizeof(buf), "ui %u  dt %ums", (unsigned)s_tick, (unsigned)dt);
    lv_label_set_text(s_status_label, buf);

    if (s_tick % 20 == 0) {
        ESP_LOGI(TAG, "test: tick %u, dt %u ms", (unsigned)s_tick, (unsigned)dt);
    }
}

/** 测试页构建（页面 enter：屏幕已由页面栈清空）。 */
static void test_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "UI Test");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_pos(title, 0, lv_pct(3));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* ---- 左列：压力测试区（x 2% 起） ---- */
    s_cnt_label = lv_label_create(scr);
    lv_label_set_text(s_cnt_label, "0");
    lv_obj_set_style_text_color(s_cnt_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_cnt_label, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(s_cnt_label, lv_pct(2), lv_pct(10));

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, lv_pct(25), lv_pct(6));
    lv_obj_set_pos(s_bar, lv_pct(2), lv_pct(30));
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    /* 小方块：12% x 20% 屏（≈100x100@800x480），x 在 0..15% 间往复位移。 */
    s_block = lv_obj_create(scr);
    lv_obj_set_size(s_block, lv_pct(12), lv_pct(20));
    lv_obj_set_pos(s_block, lv_pct(2), lv_pct(45));
    lv_obj_set_style_bg_color(s_block, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_block, 0, 0);
    lv_obj_set_style_radius(s_block, 0, 0);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "ui 0  dt 0ms");
    lv_obj_set_style_text_color(s_status_label, lv_color_black(), 0);
    lv_obj_set_pos(s_status_label, lv_pct(2), lv_pct(72));

    /* ---- 右列：按键测试区（x 52% 起） ---- */
    lv_obj_t *key_title = lv_label_create(scr);
    lv_label_set_text(key_title, "key test");
    lv_obj_set_style_text_color(key_title, lv_color_black(), 0);
    lv_obj_set_pos(key_title, lv_pct(52), lv_pct(10));

    s_last_key_label = lv_label_create(scr);
    lv_label_set_text(s_last_key_label, "last: (none)");
    lv_obj_set_style_text_color(s_last_key_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_last_key_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_last_key_label, lv_pct(52), lv_pct(18));

    s_key_count_label = lv_label_create(scr);
    lv_label_set_text(s_key_count_label, "keys: 0");
    lv_obj_set_style_text_color(s_key_count_label, lv_color_black(), 0);
    lv_obj_set_pos(s_key_count_label, lv_pct(52), lv_pct(42));

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "long press: back to home");
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_pos(hint, 0, lv_pct(90));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    s_test_timer = lv_timer_create(test_timer_cb, TEST_UI_PERIOD_MS, NULL);
    s_last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_tick = 0;
    s_key_count = 0;

    ESP_LOGI(TAG, "test screen entered (ui %u ms, block %u ms)", TEST_UI_PERIOD_MS,
             TEST_BLOCK_STEP_MS);
}

/** 测试页退出（页面 exit：删除定时器，避免离开页面后仍刷新屏幕）。 */
static void test_exit(void) {
    if (s_test_timer != NULL) {
        lv_timer_delete(s_test_timer);
        s_test_timer = NULL;
    }
    ESP_LOGI(TAG, "test screen exited");
}

/** 测试页按键处理（LVGL 线程内，由按键分发任务调用）。 */
static void test_on_key(const espaperplay_input_event_t *event) {
    s_key_count++;
    lv_label_set_text_fmt(s_last_key_label, "last: %s\nid=%u  press=%u ms",
                          espaperplay_input_key_action_str(event->key_action), event->key_id,
                          event->key_press_time_ms);
    lv_label_set_text_fmt(s_key_count_label, "keys: %u", (unsigned)s_key_count);

    if (event->key_action == ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_UP &&
        espaperplay_ui_page_depth() > 1) {
        ESP_LOGI(TAG, "test: long press up -> pop back");
        espaperplay_ui_page_pop_lv();
    }
}

/** 测试页页面实例（页面栈用）。 */
const espaperplay_ui_page_t espaperplay_ui_page_test = {test_enter, test_exit, test_on_key};
