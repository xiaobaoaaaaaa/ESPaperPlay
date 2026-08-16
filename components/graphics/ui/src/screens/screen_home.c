/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"

#include "espaperplay_clock.h"
#include "espaperplay_config.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_ui.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 主界面（最基础原型）
 * ====================================================================
 *
 * 白底 + 四个元素（布局全部使用百分比，相对屏幕尺寸——适配不同
 * 分辨率 / 比例面板）：
 *   - 标题：设备名（顶部居中，24 号）；
 *   - 系统时钟：大字号 日期 + HH:MM:SS（屏幕中上部，48 号），显示
 *     NTP 同步后的系统本地时间（clock 服务按地理位置设置时区），
 *     每 10s 刷新（小区域局部刷新——EPD 上不追求秒级实时，10s 粒度
 *     兼顾可读与刷新负担）；NTP 尚未同步（系统时间为 1970 基准）时
 *     显示占位文本；
 *   - 状态行：版本号 + 空闲堆（左上，14 号），每 10s 刷新；
 *   - 占位提示：底部居中，表明当前为原型。
 */

#define HOME_UI_PERIOD_MS 10000 /* 状态刷新周期 */
/** 判定 NTP 已同步的最小年份（未同步时系统时间停留在 1970 基准）。 */
#define HOME_CLOCK_SYNCED_YEAR 2024

static lv_obj_t *s_clock_label = NULL; /*!< 系统时钟（48 号大字，NTP 本地时间） */
static lv_obj_t *s_status_label = NULL; /*!< 版本 + 堆状态 */
static lv_timer_t *s_home_timer = NULL; /*!< 页面定时器（页面栈退出时删除） */

/** 刷新时钟 + 状态行（LVGL 线程内；进入页面与周期定时器共用，保证
 * 页面显示内容即时更新，不依赖下一次定时器触发）。 */
static void home_refresh(void) {
    char buf[64];

    /* 系统时钟：NTP 同步后的本地时间（未同步前显示占位）。 */
    struct tm local_time;
    if (espaperplay_clock_get_local_time(&local_time) == ESP_OK &&
        local_time.tm_year >= HOME_CLOCK_SYNCED_YEAR - 1900) {
        strftime(buf, sizeof(buf), "%Y-%m-%d\n%H:%M:%S", &local_time);
    } else {
        snprintf(buf, sizeof(buf), "time syncing...");
    }
    lv_label_set_text(s_clock_label, buf);

    snprintf(buf, sizeof(buf), "v%s   heap %u.%u MB", ESPAPERPLAY_VERSION,
             (unsigned)(esp_get_free_heap_size() / 1048576u),
             (unsigned)((esp_get_free_heap_size() % 1048576u) / 104857u));
    lv_label_set_text(s_status_label, buf);
}

/** 周期刷新（LVGL 线程内，lv_timer 驱动）。 */
static void home_timer_cb(lv_timer_t *timer) {
    (void)timer;
    home_refresh();
}

/** 主界面构建（页面 enter：屏幕已由页面栈清空）。 */
static void home_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    /* 标题：占满屏宽、文本居中，距顶 6% 屏高。 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESPaperPlay");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_pos(title, 0, lv_pct(6));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* 系统时钟：占满屏宽、文本居中，y = 38% 屏高。 */
    s_clock_label = lv_label_create(scr);
    lv_label_set_text(s_clock_label, "time syncing...");
    lv_obj_set_style_text_color(s_clock_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_width(s_clock_label, lv_pct(100));
    lv_obj_set_pos(s_clock_label, 0, lv_pct(38));
    lv_obj_set_style_text_align(s_clock_label, LV_TEXT_ALIGN_CENTER, 0);

    /* 状态行：左上角 (2% 屏宽, 22% 屏高)。 */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "v" ESPAPERPLAY_VERSION "  heap --.- MB");
    lv_obj_set_style_text_color(s_status_label, lv_color_black(), 0);
    lv_obj_set_pos(s_status_label, lv_pct(2), lv_pct(22));

    /* 占位提示：占满屏宽、文本居中，y = 90% 屏高。 */
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "single click: test page");
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_pos(hint, 0, lv_pct(90));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    s_home_timer = lv_timer_create(home_timer_cb, HOME_UI_PERIOD_MS, NULL);
    /* 立即刷新一次：返回主界面时即时显示当前时间 / 状态，不等 10s 定时器。 */
    home_refresh();

    ESP_LOGI(TAG, "home screen entered");
}

/** 主界面退出（页面 exit：删除定时器，避免离开页面后仍刷新屏幕）。 */
static void home_exit(void) {
    if (s_home_timer != NULL) {
        lv_timer_delete(s_home_timer);
        s_home_timer = NULL;
    }
    ESP_LOGI(TAG, "home screen exited");
}

/** 主界面按键处理（LVGL 线程内，由按键分发任务调用）：单击进入测试页。 */
static void home_on_key(const espaperplay_input_event_t *event) {
    if (event->key_action == ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK) {
        ESP_LOGI(TAG, "home: single click -> push test page");
        espaperplay_ui_page_push_lv(&espaperplay_ui_page_test);
    }
}

/** 主界面页面实例（页面栈用）。 */
const espaperplay_ui_page_t espaperplay_ui_page_home = {home_enter, home_exit, home_on_key,
                                                        NULL};

/** 展示主界面（最基础原型）。须在 espaperplay_gui_lv_start() 之后调用。 */
void espaperplay_ui_home_show(void) {
    esp_err_t err = espaperplay_ui_page_push(&espaperplay_ui_page_home);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "home screen push failed: %s", esp_err_to_name(err));
    }
}
