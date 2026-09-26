/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "espaperplay_clock.h"
#include "espaperplay_ui_util.h"
#include "espaperplay_ui_gesture.h"
#include "espaperplay_power.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"
#include "espaperplay_weather.h"
#include "icons_data.h"     /* 应用图标（Iconify -> LVGL A8 位图，生成文件） */
#include "qweather_icons.h" /* 和风天气图标（QWeather-Icons -> LVGL A8，生成文件） */

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 主界面（时钟与应用入口）
 * ====================================================================
 *
 * 布局自适应逻辑分辨率（默认竖屏 480x800 = 面板 800x480 顺时针 90°；
 * 测试页双击可循环旋转）：
 *   - 顶部状态栏（高 40px，常驻）：左侧时间 HH:MM，右侧 WiFi 状态；
 *   - 主区域：上半为居中的时钟、日期和星期，
 *     下半为原尺寸图标与名称；四个入口均匀分布，窄屏才换行；
 *   - 睡眠时由电源管理压入屏保页（screen_screensaver.c）替换整屏。
 *
 * 点击完全由本页 on_touch 手势判定（不依赖 LVGL click 事件——
 * LVGL 在无滚动对象时无论位移多大都会发送 CLICKED，滑动会误触应用）：
 *   - 按下时记录起点并做卡片命中检测（逻辑坐标）；
 *   - 释放时位移 <= 15px 且按下点在卡片内 -> 进入应用；其余 -> 无操作。
 *
 * 文字渲染使用 FreeType 中文子集字体（NotoSansSC_Regular.ttf，见
 * components/graphics/fonts）。
 *
 * 时钟与状态每秒轮询一次，但仅在显示内容实际变化时才更新标签并触发
 * EPD 刷新（分钟 / 日期 / 天气 / 版本等一有变化立即刷新，否则静默）——
 * 秒级响应时间变化、分钟切换精确到边界，同时避免无谓 EPD 刷新。
 * NTP 未同步（系统时间停留在 1970 基准）时显示占位文本。
 */

#define HOME_STATUS_H_PX ESPAPERPLAY_UI_HEADER_H /* 状态栏高度 */
#define HOME_UI_PERIOD_MS 1000 /* 时间/状态轮询周期（秒级响应；内容未变不刷新 EPD） */

#define HOME_APP_CNT 4 /* 应用数量 */

#define HOME_APP_ICON_PX 64
static int s_card_w;
static int s_card_h;

/** 判定 NTP 已同步的最小年份（未同步时系统时间停留在 1970 基准）。 */
#define HOME_CLOCK_SYNCED_YEAR 2024

/* ------------------------------------------------------------------ */
/* 应用定义                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name_zh;               /*!< 中文名（FreeType 20，图标框下方悬浮） */
    const lv_image_dsc_t *icon;        /*!< 应用图标（LVGL A8 位图，Iconify） */
    const espaperplay_ui_page_t *page; /*!< 点击进入的页面；NULL = 占位（开发中） */
} home_app_t;

static const home_app_t s_apps[HOME_APP_CNT] = {
    {"天气", &icon_weather_64, &espaperplay_ui_page_weather},
    {"阅读器", &icon_reader_64, &espaperplay_ui_page_reader},
    {"文件", &icon_files_64, &espaperplay_ui_page_files},
    {"设置", &icon_settings_64, &espaperplay_ui_page_settings},
    /* 测试页不放在主界面：经设置页「开发者」组进入（页面实例保留在 ui.h）。 */
};

/* ------------------------------------------------------------------ */
/* 页面状态                                                             */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_main = NULL;                     /*!< 主区域容器（时钟区 + 应用区） */
static espaperplay_ui_status_bar_t *s_bar = NULL;   /*!< 统一状态栏 */
static lv_obj_t *s_clock_h = NULL;                  /*!< 时钟：时（大字） */
static lv_obj_t *s_clock_m = NULL;                  /*!< 时钟：分（大字） */
static lv_obj_t *s_date_label = NULL;               /*!< 时钟：日期（M/D） */
static lv_obj_t *s_app_cards[HOME_APP_CNT];         /*!< 应用卡片（命中检测用） */
static lv_obj_t *s_app_icons[HOME_APP_CNT];         /*!< 应用卡片图标（动态换源用） */
static lv_timer_t *s_timer = NULL;                  /*!< 周期刷新定时器 */

static bool s_touch_down = false;         /*!< 手势跟踪：按下状态 */
static lv_point_t s_touch_start = {0, 0}; /*!< 手势跟踪：按下起点（逻辑坐标） */
static lv_point_t s_touch_last = {0, 0};  /*!< 手势跟踪：最近一次点 */
static int s_touch_card = -1;             /*!< 手势跟踪：按下起点命中的卡片（-1=无） */

/* 天气应用图标数据缓冲（快照较大，放 PSRAM，页面生命周期内复用）。 */
static espaperplay_weather_snapshot_t *s_weather_snap = NULL;

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */

/** 当前本地时间（NTP 已同步）或 NULL。 */
static const struct tm *home_local_time(void) {
    static struct tm s_tm;
    if (espaperplay_clock_get_local_time(&s_tm) == ESP_OK &&
        s_tm.tm_year >= HOME_CLOCK_SYNCED_YEAR - 1900) {
        return &s_tm;
    }
    return NULL;
}

static const char *const s_weekday_zh[] = {"星期日", "星期一", "星期二", "星期三",
                                           "星期四", "星期五", "星期六"};

/* ------------------------------------------------------------------ */
/* 状态栏                                                               */
/* ------------------------------------------------------------------ */

/** 状态栏：统一状态栏（左侧时间、右侧 WiFi/睡眠图标，无标题）。 */
static void home_status_bar_create(lv_obj_t *scr) {
    s_bar = espaperplay_ui_status_bar_create(scr, HOME_STATUS_H_PX, NULL, true);
}

/* ------------------------------------------------------------------ */
/* 时钟区 + 应用区                                                       */
/* ------------------------------------------------------------------ */

/** 保留 64px 应用图标；完整时钟区与四个等距入口共享居中轴线。 */
static void home_main_create(lv_obj_t *scr) {
    int32_t scr_w, scr_h;
    espaperplay_ui_screen_size(&scr_w, &scr_h);
    const bool portrait = scr_w < scr_h;
    const int margin = ESPAPERPLAY_UI_MARGIN;
    const int gap = 16;
    const int area_h = scr_h - HOME_STATUS_H_PX;
    const int hero_w = portrait ? scr_w : scr_w * 42 / 100;
    const int app_x = portrait ? 0 : hero_w;
    const int app_w = portrait ? scr_w : scr_w - hero_w;
    s_card_w = HOME_APP_ICON_PX + 4;
    s_card_h = s_card_w + 36;
    int cols = (app_w - 2 * margin + gap) / (s_card_w + gap);
    if (cols > HOME_APP_CNT)
        cols = HOME_APP_CNT;
    if (cols < 1)
        cols = 1;
    const int rows = (HOME_APP_CNT + cols - 1) / cols;
    const int app_gap = (app_w - cols * s_card_w) / (cols + 1);
    const int grid_h = rows * s_card_h + (rows - 1) * gap;
    const int grid_y = portrait ? area_h * 60 / 100 : (area_h - grid_h) / 2;

    s_main = lv_obj_create(scr);
    lv_obj_set_size(s_main, scr_w, area_h);
    lv_obj_set_pos(s_main, 0, HOME_STATUS_H_PX);
    lv_obj_set_style_bg_color(s_main, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_main, 0, 0);
    lv_obj_set_style_radius(s_main, 0, 0);
    lv_obj_set_style_pad_all(s_main, 0, 0);
    lv_obj_remove_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);

    const int hero_y = portrait ? 36 : 68;
    const int clock_px = hero_w >= 320 ? 80 : 56;
    const int digit_w = clock_px * 5 / 4;
    const int colon_w = clock_px / 2;
    const int clock_x = (hero_w - 2 * digit_w - colon_w) / 2;
    s_clock_h = espaperplay_ui_label_create(s_main, "--", clock_px, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(s_clock_h, digit_w, clock_px + 28);
    lv_obj_set_pos(s_clock_h, clock_x, hero_y + 36);
    lv_obj_t *colon = espaperplay_ui_label_create(s_main, ":", clock_px, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(colon, colon_w);
    lv_obj_set_pos(colon, clock_x + digit_w, hero_y + 30);
    s_clock_m = espaperplay_ui_label_create(s_main, "--", clock_px, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(s_clock_m, digit_w, clock_px + 28);
    lv_obj_set_pos(s_clock_m, clock_x + digit_w + colon_w, hero_y + 36);
    s_date_label = espaperplay_ui_label_create(s_main, "-- 月 -- 日", 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_date_label, hero_w);
    lv_obj_set_pos(s_date_label, 0, hero_y + clock_px + 64);

    for (int i = 0; i < HOME_APP_CNT; i++) {
        lv_obj_t *card = lv_obj_create(s_main);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(card, s_card_w, s_card_h);
        lv_obj_set_pos(card, app_x + app_gap + (i % cols) * (s_card_w + app_gap),
                       grid_y + (i / cols) * (s_card_h + gap));
        lv_obj_t *frame = lv_obj_create(card);
        espaperplay_ui_card_style(frame);
        lv_obj_set_size(frame, s_card_w, s_card_w);
        lv_obj_set_pos(frame, 0, 0);
        lv_obj_t *icon = lv_image_create(frame);
        lv_image_set_src(icon, s_apps[i].icon);
        lv_obj_center(icon);
        s_app_icons[i] = icon;
        lv_obj_t *name =
            espaperplay_ui_label_create(card, s_apps[i].name_zh, 20, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(name, s_card_w);
        lv_obj_set_pos(name, 0, s_card_w + 6);
        s_app_cards[i] = card;
    }
    ESP_LOGI(TAG, "home: %dx%d icons, 64px", cols, rows);
}

/* ------------------------------------------------------------------ */
/* 内容刷新                                                             */
/* ------------------------------------------------------------------ */

/** 刷新状态栏、时钟区、天气应用图标（时间 / WiFi / 天气）。 */
static void home_refresh(void) {
    char buf[128];
    const struct tm *tm = home_local_time();

    /* 统一状态栏（时间 / WiFi / 睡眠图标）由统一调度定时器周期刷新；
     * 此处立即刷新一次，确保返回主界面时即时显示。 */
    espaperplay_ui_status_bar_refresh(s_bar);

    /* 时钟区：时 / 分 / 星期 / 日期 */
    if (tm != NULL) {
        snprintf(buf, sizeof(buf), "%02d", tm->tm_hour);
        espaperplay_ui_label_set_text_dedup(s_clock_h, buf);
        snprintf(buf, sizeof(buf), "%02d", tm->tm_min);
        espaperplay_ui_label_set_text_dedup(s_clock_m, buf);
        snprintf(buf, sizeof(buf), "%d 月 %d 日  %s", tm->tm_mon + 1, tm->tm_mday, s_weekday_zh[tm->tm_wday]);
        espaperplay_ui_label_set_text_dedup(s_date_label, buf);
    } else {
        espaperplay_ui_label_set_text_dedup(s_clock_h, "--");
        espaperplay_ui_label_set_text_dedup(s_clock_m, "--");
        espaperplay_ui_label_set_text_dedup(s_date_label, "--/--");
    }

    /* 天气应用图标 = 和风实时天气图标（快照较大，缓冲在 PSRAM；未收录的
     * 代码回退 mdi 图标）。 */
    if (s_weather_snap == NULL) {
        s_weather_snap =
            heap_caps_malloc(sizeof(*s_weather_snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    const lv_image_dsc_t *qw_icon = NULL;
    if (s_weather_snap != NULL && espaperplay_weather_get_snapshot(s_weather_snap) == ESP_OK &&
        s_weather_snap->valid) {
        qw_icon = qweather_icon_get(s_weather_snap->now.icon);
    }
    if (s_app_icons[0] != NULL) {
        const lv_image_dsc_t *target = (qw_icon != NULL) ? qw_icon : s_apps[0].icon;
        if (lv_image_get_src(s_app_icons[0]) != (const void *)target) {
            lv_image_set_src(s_app_icons[0], target);
        }
    }
}

/** 周期刷新（LVGL 线程内，lv_timer 驱动）。 */
static void home_timer_cb(lv_timer_t *timer) {
    (void)timer;
    home_refresh();
}

/* ------------------------------------------------------------------ */
/* 应用点击                                                             */
/* ------------------------------------------------------------------ */

/** 逻辑坐标是否落在应用卡片内（卡片位于主区域，其原点即屏幕 (0, 状态栏高)）。 */
static int home_hit_app_card(const lv_point_t *p) {
    const int32_t off_y = HOME_STATUS_H_PX;
    for (int i = 0; i < HOME_APP_CNT; i++) {
        if (s_app_cards[i] == NULL) {
            continue;
        }
        const int32_t x = lv_obj_get_x(s_app_cards[i]);
        const int32_t y = lv_obj_get_y(s_app_cards[i]) + off_y;
        if (p->x >= x && p->x < x + s_card_w && p->y >= y && p->y < y + s_card_h) {
            return i;
        }
    }
    return -1;
}

/** 进入应用（点击命中，LVGL 线程内）。 */
static void home_open_app(int idx) {
    if (idx < 0 || idx >= HOME_APP_CNT) {
        return;
    }
    const home_app_t *app = &s_apps[idx];
    if (app->page != NULL) {
        ESP_LOGI(TAG, "home: open app '%s'", app->name_zh);
        espaperplay_ui_page_push_lv(app->page);
    } else {
        ESP_LOGI(TAG, "home: app '%s' not implemented yet", app->name_zh);
    }
}

/* ------------------------------------------------------------------ */
/* 页面钩子                                                             */
/* ------------------------------------------------------------------ */

/** 主界面构建（页面 enter：屏幕已由页面栈清空）。 */
static void home_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_touch_down = false;
    s_touch_card = -1;

    home_status_bar_create(scr);
    home_main_create(scr);

    s_timer = lv_timer_create(home_timer_cb, HOME_UI_PERIOD_MS, NULL);
    if (s_timer == NULL) {
        /* 定时器创建失败：只有进入时的一次刷新，无法周期更新（罕见，仅记录）。 */
        ESP_LOGE(TAG, "home: periodic refresh timer create failed");
    }
    /* 立即刷新一次：返回主界面时即时显示时间 / 状态，不等下一个定时器周期。 */
    home_refresh();

    /* 睡眠期间周期唤醒以更新时钟：对齐到分钟边界，使时钟在分钟切换时
     * 立即刷新（而非固定相位滞后达 ~60s）。刷新后由电源管理自动重新
     * 睡眠（不重置用户活动计时）。离开主界面时关闭（屏保页进入时会
     * 重新开启）。 */
    espaperplay_power_set_periodic_wakeup_minute_aligned(true);

    ESP_LOGI(TAG, "home screen entered");
}

/** 主界面退出（页面 exit：删除定时器，避免离开页面后仍刷新屏幕）。 */
static void home_exit(void) {
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    /* 关闭睡眠期间周期唤醒源：离开主界面后不再需要周期更新时钟。 */
    espaperplay_power_set_periodic_wakeup_minute_aligned(false);
    ESP_LOGI(TAG, "home screen exited");
}

/** 主界面触摸处理（LVGL 线程内）：点击进入应用。
 * 坐标经 LVGL 旋转约定映射为逻辑坐标后再判定。不依赖 LVGL click 事件：
 * LVGL 在无滚动对象时任何位移的释放都会触发 CLICKED，会误触应用卡片。 */
static void home_on_touch(const espaperplay_input_event_t *event) {
    lv_point_t p;
    espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

    if (event->touch_pressed) {
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start = p;
            s_touch_card = home_hit_app_card(&p);
        }
        s_touch_last = p;
    } else if (s_touch_down) {
        s_touch_down = false;

        const int adx = abs(s_touch_last.x - s_touch_start.x);
        const int ady = abs(s_touch_last.y - s_touch_start.y);

        /* 小位移 + 起点在卡片内：点击进入应用 */
        if (s_touch_card >= 0 && adx <= UI_GESTURE_CLICK_MAX_PX && ady <= UI_GESTURE_CLICK_MAX_PX) {
            home_open_app(s_touch_card);
        }
        s_touch_card = -1;
    }
}

/** 主界面页面实例（页面栈用；按键不参与导航——导航统一走卡片点击 / 滑动）。 */
const espaperplay_ui_page_t espaperplay_ui_page_home = {home_enter, home_exit, NULL, home_on_touch};

/** 展示主界面。须在 espaperplay_gui_lv_start() 之后调用。 */
void espaperplay_ui_home_show(void) {
    esp_err_t err = espaperplay_ui_page_push(&espaperplay_ui_page_home);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "home screen push failed: %s", esp_err_to_name(err));
    }
}
