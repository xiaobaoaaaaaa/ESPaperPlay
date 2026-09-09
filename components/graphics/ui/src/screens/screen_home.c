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
 * 主界面（安卓风格桌面）
 * ====================================================================
 *
 * 布局自适应逻辑分辨率（默认竖屏 480x800 = 面板 800x480 顺时针 90°；
 * 测试页双击可循环旋转）：
 *   - 顶部状态栏（高 30px，常驻）：左侧时间 HH:MM，右侧 WiFi 状态；
 *   - 主区域：上半为时钟区（左对齐：时/分大字、星期缩写、日期，右侧留白），
 *     下半为应用区（图标框 + 下方悬浮文字，列数随分辨率动态调整）；
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

#define HOME_STATUS_H_PX 30    /* 状态栏高度 */
#define HOME_UI_PERIOD_MS 1000 /* 时间/状态轮询周期（秒级响应；内容未变不刷新 EPD） */

#define HOME_APP_CNT 4 /* 应用数量 */

#define HOME_APP_ICON_PX 64                      /* 图标位图尺寸（A8，Iconify 生成） */
#define HOME_APP_FRAME_PX (HOME_APP_ICON_PX + 4) /* 图标框：64 + 2x2px 边框 */
#define HOME_APP_GAP_MIN 20                      /* 应用间最小间距（实际间距按分辨率均匀分摊） */
#define HOME_APP_CARD_W HOME_APP_FRAME_PX        /* 卡片宽 = 图标框宽（间距由网格统一） */
#define HOME_APP_CARD_H (HOME_APP_FRAME_PX + 32) /* 卡片高（框 + 下方文字区） */

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
static lv_obj_t *s_week_label = NULL;               /*!< 时钟：星期（英文缩写） */
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

/** FreeType 字体按需加载（缓存命中由字体组件管理；字号集合固定 4 项）。 */
/** 时钟字号按屏宽自适应：窄屏缩小防溢出。 */
static int home_clock_font_px(int32_t scr_w) {
    if (scr_w >= 400) {
        return 80;
    }
    if (scr_w >= 300) {
        return 56;
    }
    return 40;
}

/** 通用标签创建：白底黑字 + FreeType 字体 + 给定对齐。 */
/** 逻辑分辨率（旋转后，LVGL 线程内读取）。 */
/** 当前本地时间（NTP 已同步）或 NULL。 */
static const struct tm *home_local_time(void) {
    static struct tm s_tm;
    if (espaperplay_clock_get_local_time(&s_tm) == ESP_OK &&
        s_tm.tm_year >= HOME_CLOCK_SYNCED_YEAR - 1900) {
        return &s_tm;
    }
    return NULL;
}

static const char *const s_weekday_en[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

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

/** 时钟区：时/分大字（80px）+ 星期缩写 + 日期（20px），左对齐右侧留白。
 * 内容高约 HOME_CLOCK_AREA_H_BASE（800 高竖屏基准）；矮面板按可用高度压缩
 * 并同步收缩行距，见 home_clock_area_h() / home_clock_area_create()。 */
#define HOME_CLOCK_AREA_H_BASE 250
/* 时钟区内部行距（基准 250px 高）：时(0) 分(90) 周(196) 日(224)。 */
#define HOME_CLOCK_MIN_H 120 /* 内容最小高度下限：须小于矮屏 45% cap，否则反向把应用区推出屏幕 */

/** 时钟区实际占用高度：基准值与「可用高度 45%」取小，且不低于内容最小高度。
 * 防止矮竖屏面板上应用区 app_y 计算为负、与时钟区重叠。 */
static int home_clock_area_h(int32_t avail_h) {
    int h = HOME_CLOCK_AREA_H_BASE;
    const int cap = (int)(avail_h * 45 / 100);
    if (h > cap) {
        h = cap;
    }
    if (h < HOME_CLOCK_MIN_H) {
        h = HOME_CLOCK_MIN_H;
    }
    return h;
}

/** 时钟区构建：行距按实际占用高度相对基准等比收缩。 */
static void home_clock_area_create(lv_obj_t *scr, int x, int y, int area_h) {
    int32_t scr_w, scr_h;
    espaperplay_ui_screen_size(&scr_w, &scr_h);
    const int clock_px = home_clock_font_px(scr_w);

    s_clock_h = espaperplay_ui_label_create(scr, "--", clock_px, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_clock_h, x, y);

    s_clock_m = espaperplay_ui_label_create(scr, "--", clock_px, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_clock_m, x, y + area_h * 90 / HOME_CLOCK_AREA_H_BASE);

    s_week_label = espaperplay_ui_label_create(scr, "---", 20, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_week_label, x + 2, y + area_h * 196 / HOME_CLOCK_AREA_H_BASE);

    s_date_label = espaperplay_ui_label_create(scr, "--/--", 20, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_date_label, x + 2, y + area_h * 224 / HOME_CLOCK_AREA_H_BASE);
}

/** 应用卡片：图标框（边框只框图标，圆角）+ 框下方悬浮文字。 */
static lv_obj_t *home_app_card_create(lv_obj_t *parent, const home_app_t *app, int idx) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, HOME_APP_CARD_W, HOME_APP_CARD_H);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 图标框：边框只框图标 */
    lv_obj_t *frame = lv_obj_create(card);
    lv_obj_set_size(frame, HOME_APP_FRAME_PX, HOME_APP_FRAME_PX);
    lv_obj_set_pos(frame, 0, 0);
    lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_radius(frame, 12, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    /* 图标（A8 alpha 位图，LVGL 默认黑色绘制） */
    lv_obj_t *icon = lv_image_create(frame);
    lv_image_set_src(icon, app->icon);
    lv_obj_center(icon);
    s_app_icons[idx] = icon;

    /* 文字：图标框下方悬浮（无边框背景） */
    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, app->name_zh);
    lv_obj_set_style_text_color(name, lv_color_black(), 0);
    lv_font_t *font20 = espaperplay_ui_font(20);
    if (font20 != NULL) {
        lv_obj_set_style_text_font(name, font20, 0);
    }
    lv_obj_set_width(name, HOME_APP_CARD_W);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(name, 0, HOME_APP_FRAME_PX + 6);

    s_app_cards[idx] = card;
    return card;
}

/** 主区域构建：时钟区 + 应用区（列数随分辨率动态调整）。 */
static void home_main_create(lv_obj_t *scr) {
    int32_t scr_w, scr_h;
    espaperplay_ui_screen_size(&scr_w, &scr_h);

    s_main = lv_obj_create(scr);
    lv_obj_set_size(s_main, LV_PCT(100), scr_h - HOME_STATUS_H_PX);
    lv_obj_set_pos(s_main, 0, HOME_STATUS_H_PX);
    lv_obj_set_style_bg_color(s_main, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_main, 0, 0);
    lv_obj_set_style_pad_all(s_main, 0, 0);
    lv_obj_remove_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);

    const bool portrait = scr_w < scr_h;

    /* 应用区列数：按最小间距估算，随分辨率自适应（竖屏 480 -> 5 列）。
     * 实际间距 G 均匀分摊：边缘应用到屏幕边缘的间距 = 应用间间距。 */
    /* 横屏时钟区占左侧约 32.5% 屏宽（设计基准 800 -> 260），窄横屏同步收缩 */
    const int land_clock_w = scr_w * 325 / 1000;
    const int app_w = portrait ? scr_w : (scr_w - land_clock_w);
    int cols = (app_w + HOME_APP_GAP_MIN) / (HOME_APP_CARD_W + HOME_APP_GAP_MIN);
    if (cols < 1) {
        cols = 1;
    }
    const int rows = (HOME_APP_CNT + cols - 1) / cols;
    const int gap = (app_w - cols * HOME_APP_CARD_W) / (cols + 1);
    const int grid_h = rows * HOME_APP_CARD_H + (rows - 1) * gap;

    int clock_x, clock_y, app_x, app_y;
    int clock_h;

    if (portrait) {
        /* 竖屏：时钟区顶部左对齐（右侧留白）；应用区在时钟区与屏幕底之间居中。
         * 时钟区高度按可用高度压缩（矮面板防重叠），应用区钳制在不早于时钟区底。 */
        clock_x = 24;
        clock_y = 16;
        app_x = 0;
        const int avail_h = scr_h - HOME_STATUS_H_PX - 2 * clock_y;
        clock_h = home_clock_area_h(avail_h);
        app_y = clock_y + clock_h + (avail_h - clock_h - grid_h) / 2;
        if (app_y < clock_y + clock_h) {
            app_y = clock_y + clock_h;
        }
    } else {
        /* 横屏：时钟区左侧；应用区右侧垂直居中 */
        clock_x = 24;
        clock_y = 60;
        app_x = land_clock_w;
        app_y = (scr_h - HOME_STATUS_H_PX - grid_h) / 2;
        clock_h = home_clock_area_h(scr_h - HOME_STATUS_H_PX - clock_y);
    }

    /* 时钟区（挂在主区域容器上，与应用区无重叠） */
    home_clock_area_create(s_main, clock_x, clock_y, clock_h);

    /* 应用网格：从左到右按统一间距排布（边缘间距 = 应用间间距 = gap） */
    for (int i = 0; i < HOME_APP_CNT; i++) {
        const int r = i / cols;
        const int c = i % cols;
        lv_obj_t *card = home_app_card_create(s_main, &s_apps[i], i);
        lv_obj_set_pos(card, app_x + gap + c * (HOME_APP_CARD_W + gap),
                       app_y + r * (HOME_APP_CARD_H + gap));
    }

    ESP_LOGI(TAG, "home: grid %dx%d gap %d (app area %dx%d)", cols, rows, gap, app_w,
             (int)(scr_h - HOME_STATUS_H_PX - app_y));
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
        espaperplay_ui_label_set_text_dedup(s_week_label, s_weekday_en[tm->tm_wday]);
        snprintf(buf, sizeof(buf), "%d/%d", tm->tm_mon + 1, tm->tm_mday);
        espaperplay_ui_label_set_text_dedup(s_date_label, buf);
    } else {
        espaperplay_ui_label_set_text_dedup(s_clock_h, "--");
        espaperplay_ui_label_set_text_dedup(s_clock_m, "--");
        espaperplay_ui_label_set_text_dedup(s_week_label, "---");
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
        if (p->x >= x && p->x < x + HOME_APP_CARD_W && p->y >= y && p->y < y + HOME_APP_CARD_H) {
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
