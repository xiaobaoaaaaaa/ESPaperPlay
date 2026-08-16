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
#include "espaperplay_ui_touch.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 测试页（局刷压力测试 + 按键/触摸事件显示）
 * ====================================================================
 *
 * 目的 1：验证 e-paper 异步刷新链路——
 *   1) 多次快速局部刷新不阻塞 LVGL 任务（flush 只快照排队，真实刷新由
 *      worker 异步执行，LVGL 周期应稳定）；
 *   2) 局部刷新只刷脏区（worker 日志的刷新区域尺寸应随内容变化而不同：
 *      小数字 ~100px 宽 vs 大黑块位移时接近全屏）。
 *
 * 目的 2：验证按键链路——实时显示最近一次按键事件（动作 / id / 按压
 *   时长）与累计计数；双击顺时针旋转屏幕 90 度；长按松开返回上一页。
 *
 * 目的 3：验证触摸全链路（GT911 -> input 触摸队列 -> 分发任务 ->
 *   页面 on_touch）——左下方画板把触摸轨迹画成连线（保留全部中间点，
 *   抬起后另起一笔；最多 8 笔，超限自动删最旧），「clear」按钮清空
 *   画板，「back」按钮经 LVGL 指针 indev 点击返回上一页，验证控件级
 *   点击链路。墨水屏刷新慢（~370ms/次局部刷新），轨迹以连续线段的
 *   形式累积显示，而不是只显示最新坐标点。
 *
 * 布局全部使用百分比（相对屏幕尺寸），适配不同分辨率 / 比例面板：
 *   左列（压力测试区）：计数器 / 进度条 / 位移方块 / 状态行 / 触摸画板；
 *   右列（按键+触摸测试区）：最近按键事件 / 累计计数 / 触摸状态与坐标 /
 *   back + clear 按钮。
 *
 * 压力测试元素集中在左侧（包围盒 < 全屏阈值），验证稳定局部刷新链路；
 * 分散布局会触发"大面积局刷计数"策略（连续 5 次后强制全刷清残影）。
 */

#define TEST_UI_PERIOD_MS 200 /* UI 更新周期（快于 EPD 局部刷新 ~370ms，制造排队合并） */
#define TEST_BLOCK_STEP_MS (TEST_UI_PERIOD_MS * 10) /* 大黑块位移周期：2s */

/** 触摸画板：最多笔画数与单笔点数（点缓冲按笔画分配，lv_line 持有引用）。 */
#define TOUCH_PAD_STROKES_MAX 8
#define TOUCH_PAD_POINTS_PER_STROKE 512

/** 单笔轨迹（点缓冲随笔画分配，lv_line 只持引用，删除/清空时需释放）。 */
typedef struct {
    lv_obj_t *line;       /*!< lv_line 对象（挂载在画板上） */
    lv_point_precise_t *pts; /*!< 点缓冲（lv_line 持引用，需随对象释放） */
    uint16_t len;         /*!< 已写入点数 */
} touch_pad_stroke_t;

static lv_obj_t *s_cnt_label = NULL;       /*!< 计数器数字（48 号大字，小区域） */
static lv_obj_t *s_bar = NULL;             /*!< 进度条（中等区域） */
static lv_obj_t *s_block = NULL;           /*!< 大黑块（位移 -> 大面积刷新） */
static lv_obj_t *s_status_label = NULL;    /*!< 状态行 */
static lv_obj_t *s_last_key_label = NULL;  /*!< 最近一次按键事件 */
static lv_obj_t *s_key_count_label = NULL; /*!< 按键累计计数 */
static lv_obj_t *s_touch_state_label = NULL; /*!< 触摸按压状态 / 点数 / 计数 */
static lv_obj_t *s_touch_pos_label = NULL;   /*!< 最近触摸点坐标 / 笔画数 */
static lv_obj_t *s_pad = NULL;             /*!< 触摸画板容器（轨迹线挂载其上） */
static lv_timer_t *s_test_timer = NULL;    /*!< 页面定时器（页面栈退出时删除） */
static uint32_t s_tick = 0;                /*!< UI 更新计数 */
static uint32_t s_last_tick_ms = 0;        /*!< 上次更新时刻（实测周期） */
static uint32_t s_key_count = 0;           /*!< 按键计数（页面实例状态） */
static uint32_t s_touch_ev = 0;            /*!< 触摸事件计数（页面收到的事件） */
static bool s_last_touch_pressed = false;  /*!< 上次触摸按压状态（状态沿日志） */

/* 笔画环形管理：s_strokes[head..head+count) 为存活笔画，超限删最旧。 */
static touch_pad_stroke_t s_strokes[TOUCH_PAD_STROKES_MAX];
static uint8_t s_stroke_head = 0;
static uint8_t s_stroke_count = 0;
static touch_pad_stroke_t *s_cur_stroke = NULL; /*!< 当前进行中的笔画（抬起后为 NULL） */

/** back / clear 按钮点击回调（LVGL 事件回调，LVGL 线程内）。 */
static void test_back_clicked_cb(lv_event_t *e);
static void test_clear_clicked_cb(lv_event_t *e);

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

/** 清空画板：删除全部轨迹线对象并释放其点缓冲（LVGL 线程内）。 */
static void pad_clear_strokes(void) {
    for (uint8_t i = 0; i < s_stroke_count; i++) {
        touch_pad_stroke_t *stroke = &s_strokes[(s_stroke_head + i) % TOUCH_PAD_STROKES_MAX];
        if (stroke->line != NULL) {
            lv_obj_del(stroke->line); /* lv_line 持有 pts 引用：先删对象再释放缓冲 */
            stroke->line = NULL;
        }
        if (stroke->pts != NULL) {
            lv_free(stroke->pts);
            stroke->pts = NULL;
        }
        stroke->len = 0;
    }
    s_stroke_head = 0;
    s_stroke_count = 0;
    s_cur_stroke = NULL;
    ESP_LOGI(TAG, "touch pad cleared");
}

/**
 * @brief 向当前笔画追加一个画板相对坐标点（无当前笔画则新起一笔）。
 *
 * 笔画数超过 TOUCH_PAD_STROKES_MAX 时删除最旧笔画；单笔点数超过
 * TOUCH_PAD_POINTS_PER_STROKE 时丢弃后续点（等待下一笔）。
 *
 * @param p 画板相对坐标（0..画板宽/高-1，已裁剪）。
 */
static void pad_stroke_add_point(const lv_point_t *p) {
    if (s_cur_stroke == NULL) {
        /* 新笔画：环形槽满则删最旧。 */
        if (s_stroke_count >= TOUCH_PAD_STROKES_MAX) {
            touch_pad_stroke_t *oldest = &s_strokes[s_stroke_head];
            if (oldest->line != NULL) {
                lv_obj_del(oldest->line);
                oldest->line = NULL;
            }
            if (oldest->pts != NULL) {
                lv_free(oldest->pts);
                oldest->pts = NULL;
            }
            oldest->len = 0;
            s_stroke_head = (uint8_t)((s_stroke_head + 1) % TOUCH_PAD_STROKES_MAX);
            s_stroke_count--;
        }

        touch_pad_stroke_t *stroke =
            &s_strokes[(s_stroke_head + s_stroke_count) % TOUCH_PAD_STROKES_MAX];
        stroke->pts = lv_malloc(sizeof(lv_point_precise_t) * TOUCH_PAD_POINTS_PER_STROKE);
        if (stroke->pts == NULL) {
            ESP_LOGW(TAG, "touch pad: stroke point buffer alloc failed");
            return;
        }
        stroke->len = 0;
        stroke->line = lv_line_create(s_pad);
        lv_obj_set_style_line_color(stroke->line, lv_color_black(), 0);
        lv_obj_set_style_line_width(stroke->line, 3, 0);
        lv_obj_set_style_line_rounded(stroke->line, true, 0);
        lv_obj_set_pos(stroke->line, 0, 0);
        s_stroke_count++;
        s_cur_stroke = stroke;
    }

    if (s_cur_stroke->len >= TOUCH_PAD_POINTS_PER_STROKE) {
        return; /* 单笔点数上限：丢弃（继续下一笔） */
    }
    s_cur_stroke->pts[s_cur_stroke->len].x = p->x;
    s_cur_stroke->pts[s_cur_stroke->len].y = p->y;
    s_cur_stroke->len++;
    lv_line_set_points(s_cur_stroke->line, s_cur_stroke->pts, s_cur_stroke->len);
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
    lv_obj_set_pos(s_cnt_label, lv_pct(2), lv_pct(8));

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, lv_pct(25), lv_pct(6));
    lv_obj_set_pos(s_bar, lv_pct(2), lv_pct(20));
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    /* 小方块：12% x 12% 屏（≈100x60@800x480），x 在 0..15% 间往复位移。 */
    s_block = lv_obj_create(scr);
    lv_obj_set_size(s_block, lv_pct(12), lv_pct(12));
    lv_obj_set_pos(s_block, lv_pct(2), lv_pct(30));
    lv_obj_set_style_bg_color(s_block, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_block, 0, 0);
    lv_obj_set_style_radius(s_block, 0, 0);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "ui 0  dt 0ms");
    lv_obj_set_style_text_color(s_status_label, lv_color_black(), 0);
    lv_obj_set_pos(s_status_label, lv_pct(2), lv_pct(45));

    /* ---- 左列：触摸画板（轨迹绘制区） ---- */
    s_pad = lv_obj_create(scr);
    lv_obj_set_size(s_pad, lv_pct(46), lv_pct(40));
    lv_obj_set_pos(s_pad, lv_pct(2), lv_pct(53));
    lv_obj_set_style_bg_color(s_pad, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_pad, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_pad, 2, 0);
    lv_obj_set_style_radius(s_pad, 0, 0);
    lv_obj_remove_flag(s_pad, LV_OBJ_FLAG_SCROLLABLE); /* 纯绘制容器 */

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

    /* ---- 右列：触摸测试区 ---- */
    lv_obj_t *touch_title = lv_label_create(scr);
    lv_label_set_text(touch_title, "touch test");
    lv_obj_set_style_text_color(touch_title, lv_color_black(), 0);
    lv_obj_set_pos(touch_title, lv_pct(52), lv_pct(54));

    s_touch_state_label = lv_label_create(scr);
    lv_label_set_text(s_touch_state_label, "released  ev=0");
    lv_obj_set_style_text_color(s_touch_state_label, lv_color_black(), 0);
    lv_obj_set_pos(s_touch_state_label, lv_pct(52), lv_pct(62));

    s_touch_pos_label = lv_label_create(scr);
    lv_label_set_text(s_touch_pos_label, "x=- y=-");
    lv_obj_set_style_text_color(s_touch_pos_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_touch_pos_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_touch_pos_label, lv_pct(52), lv_pct(70));

    /* back 按钮：经 LVGL 指针 indev 点击返回上一页（验证控件级点击链路）。 */
    lv_obj_t *back_btn = lv_button_create(scr);
    lv_obj_set_size(back_btn, lv_pct(20), lv_pct(8));
    lv_obj_set_pos(back_btn, lv_pct(52), lv_pct(78));
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, test_back_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* clear 按钮：清空画板轨迹（同样经 LVGL 指针 indev 点击）。 */
    lv_obj_t *clear_btn = lv_button_create(scr);
    lv_obj_set_size(clear_btn, lv_pct(20), lv_pct(8));
    lv_obj_set_pos(clear_btn, lv_pct(74), lv_pct(78));
    lv_obj_t *clear_label = lv_label_create(clear_btn);
    lv_label_set_text(clear_label, "clear");
    lv_obj_center(clear_label);
    lv_obj_add_event_cb(clear_btn, test_clear_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "dbl-click: rotate\nlong-press: back");
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_width(hint, lv_pct(46));
    lv_obj_set_pos(hint, lv_pct(52), lv_pct(87));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    s_test_timer = lv_timer_create(test_timer_cb, TEST_UI_PERIOD_MS, NULL);
    s_last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_tick = 0;
    s_key_count = 0;
    s_touch_ev = 0;
    s_last_touch_pressed = false;
    memset(s_strokes, 0, sizeof(s_strokes));
    s_stroke_head = 0;
    s_stroke_count = 0;
    s_cur_stroke = NULL;

    ESP_LOGI(TAG, "test screen entered (ui %u ms, block %u ms)", TEST_UI_PERIOD_MS,
             TEST_BLOCK_STEP_MS);
}

/** 测试页退出（页面 exit：删除定时器并释放画板笔画，避免离开页面后仍刷新屏幕）。 */
static void test_exit(void) {
    if (s_test_timer != NULL) {
        lv_timer_delete(s_test_timer);
        s_test_timer = NULL;
    }
    pad_clear_strokes();
    s_pad = NULL;
    ESP_LOGI(TAG, "test screen exited");
}

/** 测试页按键处理（LVGL 线程内，由按键分发任务调用）。 */
static void test_on_key(const espaperplay_input_event_t *event) {
    s_key_count++;
    lv_label_set_text_fmt(s_last_key_label, "last: %s\nid=%u  press=%u ms",
                          espaperplay_input_key_action_str(event->key_action), event->key_id,
                          event->key_press_time_ms);
    lv_label_set_text_fmt(s_key_count_label, "keys: %u", (unsigned)s_key_count);

    if (event->key_action == ESPAPERPLAY_INPUT_KEY_ACTION_DOUBLE_CLICK) {
        /* 双击：屏幕顺时针旋转 90 度（0 -> 90 -> 180 -> 270 -> 0 循环）。
         * LVGL 只交换逻辑分辨率（如 800x480 -> 480x800）并整屏失效重绘，
         * 像素旋转由移植层 flush 回调（lvgl_port.c）完成。 */
        lv_display_t *disp = lv_display_get_default();
        if (disp != NULL) {
            const lv_display_rotation_t next =
                (lv_display_rotation_t)((lv_display_get_rotation(disp) + 1) % 4);
            lv_display_set_rotation(disp, next);
            ESP_LOGI(TAG, "test: double click -> rotate cw %d deg", (int)next * 90);
        }
    }

    if (event->key_action == ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_UP &&
        espaperplay_ui_page_depth() > 1) {
        ESP_LOGI(TAG, "test: long press up -> pop back");
        espaperplay_ui_page_pop_lv();
    }
}

/** back 按钮点击：返回上一页（LVGL 事件回调，LVGL 线程内）。 */
static void test_back_clicked_cb(lv_event_t *e) {
    (void)e;
    if (espaperplay_ui_page_depth() > 1) {
        ESP_LOGI(TAG, "test: back button clicked -> pop back");
        espaperplay_ui_page_pop_lv();
    }
}

/** clear 按钮点击：清空画板轨迹（LVGL 事件回调，LVGL 线程内）。 */
static void test_clear_clicked_cb(lv_event_t *e) {
    (void)e;
    pad_clear_strokes();
    lv_label_set_text_fmt(s_touch_state_label, "cleared  ev=%u", (unsigned)s_touch_ev);
    lv_label_set_text(s_touch_pos_label, "x=- y=-");
}

/**
 * 测试页触摸处理（LVGL 线程内，由输入分发任务批量逐条转发）。
 *
 * 轨迹绘制：面板物理坐标 -> LVGL 逻辑坐标（旋转后仍对齐）-> 画板相对
 * 坐标（裁剪到画板内），逐点追加到当前笔画并连线；释放后收笔，下一次
 * 按下另起一笔——中间所有识别到的位置都累积显示在画板上。
 */
static void test_on_touch(const espaperplay_input_event_t *event) {
    s_touch_ev++;
    const bool pressed = event->touch_pressed != 0;

    if (!pressed) {
        s_cur_stroke = NULL; /* 收笔 */
        lv_label_set_text_fmt(s_touch_state_label, "released  ev=%u",
                              (unsigned)s_touch_ev);
        lv_label_set_text_fmt(s_touch_pos_label, "x=- y=-  strokes=%u", s_stroke_count);
        if (s_last_touch_pressed) {
            ESP_LOGI(TAG, "touch: released (ev %u, strokes %u)", (unsigned)s_touch_ev,
                     s_stroke_count);
        }
    } else {
        lv_point_t p;
        espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

        /* 画板相对坐标并裁剪到画板内。 */
        const int32_t pad_x = p.x - lv_obj_get_x(s_pad);
        const int32_t pad_y = p.y - lv_obj_get_y(s_pad);
        lv_point_t q;
        q.x = (int32_t)(pad_x < 0 ? 0 : pad_x);
        q.y = (int32_t)(pad_y < 0 ? 0 : pad_y);
        if (q.x >= lv_obj_get_width(s_pad)) {
            q.x = lv_obj_get_width(s_pad) - 1;
        }
        if (q.y >= lv_obj_get_height(s_pad)) {
            q.y = lv_obj_get_height(s_pad) - 1;
        }

        pad_stroke_add_point(&q);

        lv_label_set_text_fmt(s_touch_state_label, "pressed  pts=%u  ev=%u",
                              event->touch_points, (unsigned)s_touch_ev);
        lv_label_set_text_fmt(s_touch_pos_label, "x=%u y=%u  strokes=%u", event->point.x,
                              event->point.y, s_stroke_count);
        if (!s_last_touch_pressed) {
            ESP_LOGI(TAG, "touch: pressed (%u pts, x=%u y=%u, stroke start)", event->touch_points,
                     event->point.x, event->point.y);
        }
    }
    s_last_touch_pressed = pressed;
}

/** 测试页页面实例（页面栈用）。 */
const espaperplay_ui_page_t espaperplay_ui_page_test = {test_enter, test_exit, test_on_key,
                                                        test_on_touch};
