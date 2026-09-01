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
#include "esp_system.h"

#include "espaperplay_clock.h"
#include "espaperplay_config.h"
#include "espaperplay_fonts.h"
#include "espaperplay_power.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"
#include "espaperplay_weather.h"
#include "espaperplay_wifi.h"
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
 *   - 主区域含两个子页（左右滑动切换，底部圆点指示）：
 *       页 0：上半为时钟区（左对齐：时/分大字、星期缩写、日期，右侧留白），
 *             下半为应用区（图标框 + 下方悬浮文字，列数随分辨率动态调整）；
 *       页 1：大字号时钟 + 日期 + 天气摘要 + 版本/堆状态；
 *   - 底部中央：页面指示点（当前页实心）。
 *
 * 点击与滑动完全由本页 on_touch 手势判定（不依赖 LVGL click 事件——
 * LVGL 在无滚动对象时无论位移多大都会发送 CLICKED，滑动会误触应用）：
 *   - 按下时记录起点并做卡片命中检测（逻辑坐标）；
 *   - 释放时横向位移 > 90px 且横向为主 -> 切页；
 *     位移 <= 15px 且按下点在卡片内 -> 进入应用；
 *     其余 -> 无操作。
 *
 * 文字渲染使用 FreeType 中文子集字体（NotoSansSC_Regular.ttf，见
 * components/graphics/fonts），字号 16 / 20 / 24 / 64（共 4 项，
 * 与字体组件缓存容量一致）。
 *
 * 时钟与状态每秒轮询一次，但仅在显示内容实际变化时才更新标签并触发
 * EPD 刷新（分钟 / 日期 / 天气 / 版本等一有变化立即刷新，否则静默）——
 * 秒级响应时间变化、分钟切换精确到边界，同时避免无谓 EPD 刷新。
 * NTP 未同步（系统时间停留在 1970 基准）时显示占位文本。
 */

#define HOME_STATUS_H_PX 30       /* 状态栏高度 */
#define HOME_SWIPE_THRESH_PX 90   /* 滑动切页位移阈值 */
#define HOME_CLICK_MAX_PX 15      /* 点击允许的最大位移（防抖） */
#define HOME_SWIPE_MIN_RATIO 1.2f /* 横向位移 / 纵向位移 最小比例 */
#define HOME_UI_PERIOD_MS 1000    /* 时间/状态轮询周期（秒级响应；内容未变不刷新 EPD） */

#define HOME_APP_CNT 4  /* 应用数量 */
#define HOME_PAGE_CNT 2 /* 主区域子页数 */

#define HOME_APP_ICON_PX 64                      /* 图标位图尺寸（A8，Iconify 生成） */
#define HOME_APP_FRAME_PX (HOME_APP_ICON_PX + 4) /* 图标框：64 + 2x2px 边框 */
#define HOME_APP_GAP_MIN 20                      /* 应用间最小间距（实际间距按分辨率均匀分摊） */
#define HOME_APP_CARD_W HOME_APP_FRAME_PX        /* 卡片宽 = 图标框宽（间距由网格统一） */
#define HOME_APP_CARD_H (HOME_APP_FRAME_PX + 32) /* 卡片高（框 + 下方文字区） */

#define HOME_FONT_NAME                                                                             \
    (espaperplay_system_get_config()                                                               \
         ->selected_font) /* 当前选用字体（SD 优先，缺则回退 Flash 子集） */

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

static lv_obj_t *s_page0 = NULL;                  /*!< 子页 0：时钟 + 应用 */
static lv_obj_t *s_page1 = NULL;                  /*!< 子页 1：时钟信息 */
static espaperplay_ui_status_bar_t *s_bar = NULL; /*!< 统一状态栏 */
static lv_obj_t *s_clock_h = NULL;                /*!< 页 0 时钟：时（大字） */
static lv_obj_t *s_clock_m = NULL;                /*!< 页 0 时钟：分（大字） */
static lv_obj_t *s_week_label = NULL;             /*!< 页 0 时钟：星期（英文缩写） */
static lv_obj_t *s_date_label = NULL;             /*!< 页 0 时钟：日期（M/D） */
static lv_obj_t *s_app_cards[HOME_APP_CNT];       /*!< 应用卡片（命中检测用） */
static lv_obj_t *s_app_icons[HOME_APP_CNT];       /*!< 应用卡片图标（动态换源用） */
static lv_obj_t *s_clock_big = NULL;              /*!< 页 1：大时钟 */
static lv_obj_t *s_info_date = NULL;              /*!< 页 1：日期 */
static lv_obj_t *s_info_weather = NULL;           /*!< 页 1：天气摘要 */
static lv_obj_t *s_info_footer = NULL;            /*!< 页 1：版本 / 堆 / 提示 */
static lv_obj_t *s_dots[HOME_PAGE_CNT];           /*!< 页面指示点 */
static lv_timer_t *s_timer = NULL;                /*!< 周期刷新定时器 */

static int s_page = 0;                    /*!< 当前子页索引 */
static bool s_touch_down = false;         /*!< 手势跟踪：按下状态 */
static lv_point_t s_touch_start = {0, 0}; /*!< 手势跟踪：按下起点（逻辑坐标） */
static lv_point_t s_touch_last = {0, 0};  /*!< 手势跟踪：最近一次点 */
static int s_touch_card = -1;             /*!< 手势跟踪：按下起点命中的卡片（-1=无） */

/* 页 1 天气行数据缓冲（快照较大，放 PSRAM，页面生命周期内复用）。 */
static espaperplay_weather_snapshot_t *s_weather_snap = NULL;

/* ---- 各标签「上次已显示的文本」缓存：内容未变化时不重复 set_text，
 *      避免 LVGL 无效化 -> EPD 无谓局刷（秒级轮询下尤为关键）。 ---- */
static char s_prev_clock_h[4] = "";        /*!< 页 0 时钟：时 */
static char s_prev_clock_m[4] = "";        /*!< 页 0 时钟：分 */
static char s_prev_week[8] = "";           /*!< 页 0 时钟：星期（英文缩写） */
static char s_prev_date[16] = "";          /*!< 页 0 时钟：日期 M/D */
static char s_prev_clock_big[8] = "";      /*!< 页 1 大时钟 HH:MM */
static char s_prev_info_date[64] = "";     /*!< 页 1 日期（年月日 星期） */
static char s_prev_info_weather[256] = ""; /*!< 页 1 天气摘要 */
static char s_prev_info_footer[128] = "";  /*!< 页 1 版本 / 堆 / 提示 */

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */

/** FreeType 字体按需加载（缓存命中由字体组件管理；字号集合固定 4 项）。 */
static lv_font_t *home_font(int size_px) {
    return espaperplay_fonts_load(HOME_FONT_NAME, (uint32_t)size_px, ESPAPERPLAY_FONT_STYLE_NORMAL);
}

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
static lv_obj_t *home_label_create(lv_obj_t *parent, const char *text, int font_px,
                                   lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_font_t *font = home_font(font_px);
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_width(label, LV_PCT(100));
    return label;
}

/** 逻辑分辨率（旋转后，LVGL 线程内读取）。 */
static void home_screen_size(int32_t *out_w, int32_t *out_h) {
    lv_display_t *disp = lv_display_get_default();
    *out_w = lv_display_get_horizontal_resolution(disp);
    *out_h = lv_display_get_vertical_resolution(disp);
}

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
static const char *const s_weekday_zh[] = {"日", "一", "二", "三", "四", "五", "六"};

/* ------------------------------------------------------------------ */
/* 状态栏                                                               */
/* ------------------------------------------------------------------ */

/** 状态栏：统一状态栏（左侧时间、右侧 WiFi/睡眠图标，无标题）。 */
static void home_status_bar_create(lv_obj_t *scr) {
    s_bar = espaperplay_ui_status_bar_create(scr, HOME_STATUS_H_PX, NULL, true);
}

/* ------------------------------------------------------------------ */
/* 页 0：时钟区 + 应用区                                                 */
/* ------------------------------------------------------------------ */

/** 页 0 时钟区：时/分大字（80px）+ 星期缩写 + 日期（20px），左对齐右侧留白。
 * 内容高约 HOME_CLOCK_AREA_H_BASE（800 高竖屏基准）；矮面板按可用高度压缩
 * 并同步收缩行距，见 home_clock_area_h() / home_clock_area_create()。 */
#define HOME_CLOCK_AREA_H_BASE 250
/* 时钟区内部行距（基准 250px 高）：时(0) 分(90) 周(196) 日(224)。 */
#define HOME_CLOCK_MIN_H 240 /* 内容最小高度：分(80px)与周(20px)不重叠、日期行底不裁切的下限 */

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
    home_screen_size(&scr_w, &scr_h);
    const int clock_px = home_clock_font_px(scr_w);

    s_clock_h = home_label_create(scr, "--", clock_px, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_clock_h, x, y);

    s_clock_m = home_label_create(scr, "--", clock_px, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_clock_m, x, y + area_h * 90 / HOME_CLOCK_AREA_H_BASE);

    s_week_label = home_label_create(scr, "---", 20, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_week_label, x + 2, y + area_h * 196 / HOME_CLOCK_AREA_H_BASE);

    s_date_label = home_label_create(scr, "--/--", 20, LV_TEXT_ALIGN_LEFT);
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
    lv_font_t *font20 = home_font(20);
    if (font20 != NULL) {
        lv_obj_set_style_text_font(name, font20, 0);
    }
    lv_obj_set_width(name, HOME_APP_CARD_W);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(name, 0, HOME_APP_FRAME_PX + 6);

    s_app_cards[idx] = card;
    return card;
}

/** 页 0 构建：时钟区 + 应用区（列数随分辨率动态调整）。 */
static void home_page0_create(lv_obj_t *scr) {
    int32_t scr_w, scr_h;
    home_screen_size(&scr_w, &scr_h);

    s_page0 = lv_obj_create(scr);
    lv_obj_set_size(s_page0, LV_PCT(100), scr_h - HOME_STATUS_H_PX);
    lv_obj_set_pos(s_page0, 0, HOME_STATUS_H_PX);
    lv_obj_set_style_bg_color(s_page0, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_page0, 0, 0);
    lv_obj_set_style_pad_all(s_page0, 0, 0);
    lv_obj_remove_flag(s_page0, LV_OBJ_FLAG_SCROLLABLE);

    const bool portrait = scr_w < scr_h;

    /* 应用区列数：按最小间距估算，随分辨率自适应（竖屏 480 -> 5 列）。
     * 实际间距 G 均匀分摊：边缘应用到屏幕边缘的间距 = 应用间间距。 */
    const int app_w = portrait ? scr_w : (scr_w - 260);
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
        app_x = 260;
        app_y = (scr_h - HOME_STATUS_H_PX - grid_h) / 2;
        clock_h = home_clock_area_h(scr_h - HOME_STATUS_H_PX - clock_y);
    }

    /* 时钟区（挂在页容器上，与应用区无重叠） */
    home_clock_area_create(s_page0, clock_x, clock_y, clock_h);

    /* 应用网格：从左到右按统一间距排布（边缘间距 = 应用间间距 = gap） */
    for (int i = 0; i < HOME_APP_CNT; i++) {
        const int r = i / cols;
        const int c = i % cols;
        lv_obj_t *card = home_app_card_create(s_page0, &s_apps[i], i);
        lv_obj_set_pos(card, app_x + gap + c * (HOME_APP_CARD_W + gap),
                       app_y + r * (HOME_APP_CARD_H + gap));
    }

    ESP_LOGI(TAG, "home: page0 grid %dx%d gap %d (app area %dx%d)", cols, rows, gap, app_w,
             (int)(scr_h - HOME_STATUS_H_PX - app_y));
}

/* ------------------------------------------------------------------ */
/* 页 1：时钟信息                                                       */
/* ------------------------------------------------------------------ */

/** 子页 1：大时钟 + 日期 + 天气摘要 + 版本状态（垂直百分比布局，横竖屏自适应）。 */
static void home_page1_create(lv_obj_t *scr) {
    int32_t scr_w, scr_h;
    home_screen_size(&scr_w, &scr_h);

    s_page1 = lv_obj_create(scr);
    lv_obj_set_size(s_page1, LV_PCT(100), scr_h - HOME_STATUS_H_PX);
    lv_obj_set_pos(s_page1, 0, HOME_STATUS_H_PX);
    lv_obj_set_style_bg_color(s_page1, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_page1, 0, 0);
    lv_obj_set_style_pad_all(s_page1, 0, 0);
    lv_obj_remove_flag(s_page1, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t area_h = scr_h - HOME_STATUS_H_PX;
    const int clock_px = home_clock_font_px(scr_w);

    s_clock_big = home_label_create(s_page1, "--:--", clock_px, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_clock_big, 0, area_h * 13 / 100);

    s_info_date = home_label_create(s_page1, "", 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_info_date, 0, area_h * 42 / 100);

    s_info_weather = home_label_create(s_page1, "", 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_info_weather, 0, area_h * 54 / 100);

    s_info_footer = home_label_create(s_page1, "", 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_info_footer, 0, area_h * 72 / 100);
}

/** 页面指示点（底部中央，当前页实心黑、其余空心）。 */
static void home_dots_create(lv_obj_t *scr) {
    int32_t scr_w, scr_h;
    home_screen_size(&scr_w, &scr_h);

    for (int i = 0; i < HOME_PAGE_CNT; i++) {
        s_dots[i] = lv_obj_create(scr);
        lv_obj_set_size(s_dots[i], 10, 10);
        lv_obj_set_pos(s_dots[i], scr_w / 2 + (i - (HOME_PAGE_CNT - 1) / 2) * 28 - 5, scr_h - 20);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_dots[i], 1, 0);
        lv_obj_set_style_border_color(s_dots[i], lv_color_black(), 0);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }
}

/* ------------------------------------------------------------------ */
/* 内容刷新                                                             */
/* ------------------------------------------------------------------ */

/** 仅当文本实际变化时才更新标签（EPD 上避免无谓刷新）。
 *  @param label  目标标签（本页构建期内非 NULL）。
 *  @param prev   该标签上次已显示的文本缓存（更新时同步写入）。
 *  @param text   本次要显示的新文本。 */
static void home_label_update(lv_obj_t *label, char *prev, size_t prev_size, const char *text) {
    if (strcmp(prev, text) == 0) {
        return; /* 内容未变：跳过 set_text，不触发 LVGL 无效化 */
    }
    strlcpy(prev, text, prev_size);
    lv_label_set_text(label, text);
}

/** 刷新状态栏、页 0 时钟区、页 1 信息（时间 / WiFi / 天气 / 版本）。 */
static void home_refresh(void) {
    char buf[128];
    const struct tm *tm = home_local_time();

    /* 统一状态栏（时间 / WiFi / 睡眠图标）由统一调度定时器周期刷新；
     * 此处立即刷新一次，确保返回主界面时即时显示。 */
    espaperplay_ui_status_bar_refresh(s_bar);

    /* 页 0 时钟区：时 / 分 / 星期 / 日期 */
    if (tm != NULL) {
        snprintf(buf, sizeof(buf), "%02d", tm->tm_hour);
        home_label_update(s_clock_h, s_prev_clock_h, sizeof(s_prev_clock_h), buf);
        snprintf(buf, sizeof(buf), "%02d", tm->tm_min);
        home_label_update(s_clock_m, s_prev_clock_m, sizeof(s_prev_clock_m), buf);
        home_label_update(s_week_label, s_prev_week, sizeof(s_prev_week),
                          s_weekday_en[tm->tm_wday]);
        snprintf(buf, sizeof(buf), "%d/%d", tm->tm_mon + 1, tm->tm_mday);
        home_label_update(s_date_label, s_prev_date, sizeof(s_prev_date), buf);
    } else {
        home_label_update(s_clock_h, s_prev_clock_h, sizeof(s_prev_clock_h), "--");
        home_label_update(s_clock_m, s_prev_clock_m, sizeof(s_prev_clock_m), "--");
        home_label_update(s_week_label, s_prev_week, sizeof(s_prev_week), "---");
        home_label_update(s_date_label, s_prev_date, sizeof(s_prev_date), "--/--");
    }

    /* 页 1 大时钟 + 日期 */
    if (tm != NULL) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
        home_label_update(s_clock_big, s_prev_clock_big, sizeof(s_prev_clock_big), buf);
        snprintf(buf, sizeof(buf), "%04d年%02d月%02d日 星期%s", tm->tm_year + 1900, tm->tm_mon + 1,
                 tm->tm_mday, s_weekday_zh[tm->tm_wday]);
    } else {
        home_label_update(s_clock_big, s_prev_clock_big, sizeof(s_prev_clock_big), "--:--");
        snprintf(buf, sizeof(buf), "正在同步时间…");
    }
    home_label_update(s_info_date, s_prev_info_date, sizeof(s_prev_info_date), buf);

    /* 页 1 天气摘要（快照较大，缓冲在 PSRAM）+ 天气应用图标（实时天气图标） */
    if (s_weather_snap == NULL) {
        s_weather_snap =
            heap_caps_malloc(sizeof(*s_weather_snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    const lv_image_dsc_t *qw_icon = NULL;
    if (s_weather_snap != NULL && espaperplay_weather_get_snapshot(s_weather_snap) == ESP_OK &&
        s_weather_snap->valid) {
        char wbuf[256]; /* location_name 最长 127 字符 */
        snprintf(wbuf, sizeof(wbuf), "%s · %s %s℃  湿度 %s%%", s_weather_snap->location_name,
                 s_weather_snap->now.text, s_weather_snap->now.temp, s_weather_snap->now.humidity);
        home_label_update(s_info_weather, s_prev_info_weather, sizeof(s_prev_info_weather), wbuf);

        /* 天气应用图标 = 和风实时天气图标（未收录的代码回退 mdi 图标） */
        qw_icon = qweather_icon_get(s_weather_snap->now.icon);
    } else {
        snprintf(buf, sizeof(buf), "天气：未配置或不可用（Web 页面设置）");
        home_label_update(s_info_weather, s_prev_info_weather, sizeof(s_prev_info_weather), buf);
    }
    if (s_app_icons[0] != NULL) {
        const lv_image_dsc_t *target = (qw_icon != NULL) ? qw_icon : s_apps[0].icon;
        if (lv_image_get_src(s_app_icons[0]) != (const void *)target) {
            lv_image_set_src(s_app_icons[0], target);
        }
    }

    /* 页 1 版本 / 堆 / 操作提示 */
    snprintf(buf, sizeof(buf), "v%s   heap %u.%u MB   左右滑动切换页面", ESPAPERPLAY_VERSION,
             (unsigned)(esp_get_free_heap_size() / 1048576u),
             (unsigned)((esp_get_free_heap_size() % 1048576u) / 104857u));
    home_label_update(s_info_footer, s_prev_info_footer, sizeof(s_prev_info_footer), buf);
}

/** 周期刷新（LVGL 线程内，lv_timer 驱动）。 */
static void home_timer_cb(lv_timer_t *timer) {
    (void)timer;
    home_refresh();
}

/* ------------------------------------------------------------------ */
/* 页面切换 / 手势                                                      */
/* ------------------------------------------------------------------ */

/** 切换子页：容器显隐 + 指示点刷新（LVGL 线程内）。 */
static void home_show_page(int idx) {
    if (idx < 0 || idx >= HOME_PAGE_CNT || idx == s_page) {
        return;
    }
    s_page = idx;

    if (idx == 0) {
        lv_obj_add_flag(s_page1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_page0, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_page0, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_page1, LV_OBJ_FLAG_HIDDEN);
        home_refresh(); /* 进入信息页立即刷新（时钟/天气不依赖下一次定时器） */
    }

    for (int i = 0; i < HOME_PAGE_CNT; i++) {
        lv_obj_set_style_bg_color(s_dots[i], i == idx ? lv_color_black() : lv_color_white(), 0);
    }
    ESP_LOGI(TAG, "home: page %d", idx);
}

/** 逻辑坐标是否落在应用卡片内（卡片位于 s_page0，其原点即屏幕 (0, 状态栏高)）。 */
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

    s_page = 0;
    s_touch_down = false;
    s_touch_card = -1;

    home_status_bar_create(scr);
    home_page0_create(scr);
    home_page1_create(scr);
    home_dots_create(scr);

    /* 初始显示页 0；页 1 隐藏。 */
    lv_obj_add_flag(s_page1, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < HOME_PAGE_CNT; i++) {
        lv_obj_set_style_bg_color(s_dots[i], i == 0 ? lv_color_black() : lv_color_white(), 0);
    }

    /* 标签为本次进入新建（初始占位文本）：清空文本缓存，首次刷新强制落数据。 */
    s_prev_clock_h[0] = '\0';
    s_prev_clock_m[0] = '\0';
    s_prev_week[0] = '\0';
    s_prev_date[0] = '\0';
    s_prev_clock_big[0] = '\0';
    s_prev_info_date[0] = '\0';
    s_prev_info_weather[0] = '\0';
    s_prev_info_footer[0] = '\0';

    s_timer = lv_timer_create(home_timer_cb, HOME_UI_PERIOD_MS, NULL);
    if (s_timer == NULL) {
        /* 定时器创建失败：只有进入时的一次刷新，无法周期更新（罕见，仅记录）。 */
        ESP_LOGE(TAG, "home: periodic refresh timer create failed");
    }
    /* 立即刷新一次：返回主界面时即时显示时间 / 状态，不等下一个定时器周期。 */
    home_refresh();

    /* 睡眠期间周期唤醒以更新时钟：对齐到分钟边界，使时钟在分钟切换时
     * 立即刷新（而非固定相位滞后达 ~60s）。刷新后由电源管理自动重新
     * 睡眠（不重置用户活动计时）。离开主界面时关闭。 */
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

/** 主界面触摸处理（LVGL 线程内）：点击进入应用 / 横向滑动切换子页。
 * 坐标经 LVGL 旋转约定映射为逻辑坐标后再判定。不依赖 LVGL click 事件：
 * LVGL 在无滚动对象时任何位移的释放都会触发 CLICKED，会误触应用卡片。 */
static void home_on_touch(const espaperplay_input_event_t *event) {
    lv_point_t p;
    espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

    if (event->touch_pressed) {
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start = p;
            /* 仅页 0（应用网格）响应卡片点击；页 1 上不做命中检测。 */
            s_touch_card = (s_page == 0) ? home_hit_app_card(&p) : -1;
        }
        s_touch_last = p;
    } else if (s_touch_down) {
        s_touch_down = false;

        const int dx = s_touch_last.x - s_touch_start.x;
        const int dy = s_touch_last.y - s_touch_start.y;
        const int adx = abs(dx);
        const int ady = abs(dy);

        if (adx > HOME_SWIPE_THRESH_PX && adx > ady * HOME_SWIPE_MIN_RATIO) {
            /* 横向滑动：切页 */
            home_show_page(s_page + (dx < 0 ? 1 : -1));
        } else if (s_touch_card >= 0 && adx <= HOME_CLICK_MAX_PX && ady <= HOME_CLICK_MAX_PX) {
            /* 小位移 + 起点在卡片内：点击进入应用 */
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
