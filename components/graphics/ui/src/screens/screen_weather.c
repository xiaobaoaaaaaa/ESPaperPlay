/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "espaperplay_clock.h"
#include "espaperplay_fonts.h"
#include "espaperplay_input.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"
#include "espaperplay_weather.h"
#include "icons_data.h"
#include "qweather_icons.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 天气页（和风天气数据展示，UI 风格与主界面统一）
 * ====================================================================
 *
 * 布局：顶部标题栏（位置名居中） + 3 个子页（左右滑动切换，底部圆点）：
 *   子页 0「实时」：温度大字（96px，右上单位 / 右侧体感两行）+ 天气与
 *                 今日高低温 + 预警长条（点击弹详情卡片）+ 未来 24h
 *                 气温平滑曲线（曲线正上方标注温度值，横向滚动查看）；
 *   子页 1「7 天 + 详情」：未来 7 天最高/最低气温双曲线（最高温标注在
 *                 曲线上方、最低温在下方，横向滚动）+ 每列日期代称/图标
 *                 + 次要信息卡片（湿度/风/降水/气压/能见度/体感，附图标）；
 *   子页 2「天文 + 指数」：日出日落/月出月落弧线（太阳/月亮图标按当前
 *                 时间定位）+ 天气指数列表（穿衣/洗车等）。
 *
 * 交互：
 *   - 屏幕左/右边缘向内滑动返回主页（安卓边缘手势）；
 *   - 屏幕中间左右滑动切换子页（图表滚动区内的滑动留给 LVGL 横向滚动）；
 *   - 点击预警长条弹出预警详情卡片，点击任意处关闭；
 *   - 物理按键单击返回。
 *
 * 图表约定：仅显示平滑曲线——无网格刻度（div line = 0）、不画数据点；
 * 平滑使用 Catmull-Rom 插值并对每段输出做端点范围钳制（保证最高温曲线
 * 永不低于最低温曲线，避免过冲交叉）；24h 曲线渲染到最后一列标注点
 * （21h），右端与末列对齐，折线不超出温度/图标列。
 *
 * 风格统一（主界面设计基准）：FreeType 中文（16 / 20 / 24 / 96 四档 +
 * 主页共用 80 档，字体缓存 6 项）、和风天气图标、圆角卡片（与屏幕边缘
 * 保持间距）、白底黑字。除图表滚动容器外，所有对象禁用 LVGL 滚动。
 *
 * 数据：weather 服务内存快照（后台任务周期刷新），页面定时器 30s 刷新，
 * 数据更新时间未变化时跳过标签更新（EPD 避免无谓刷新）；快照尚不可用
 * （启动初期 / 刷新失败）时页面切到秒级快速轮询并周期性催促后台任务，
 * 天气数据获取成功后即刻更新页面。
 */

#define WEATHER_UI_PERIOD_MS 30000 /* 页面定时器刷新周期 */
#define WEATHER_UI_RETRY_MS 1000   /* 快照还不可用时（启动初期/刷新失败）的快速重试周期 */
#define WEATHER_BAR_H_PX 30        /* 标题栏高度 */
#define WEATHER_EDGE_PX 48         /* 边缘滑动触发宽度 */
#define WEATHER_EDGE_SWIPE_PX 70   /* 边缘向内滑动位移阈值 */
#define WEATHER_SWIPE_PX 90        /* 子页切换位移阈值 */
#define WEATHER_MARGIN 24          /* 卡片与屏幕边缘间距 */

#define WEATHER_SUBPAGE_CNT 3
#define WEATHER_HOURLY_CNT 24 /* 未来 24 小时（数据点数） */
#define WEATHER_HOURLY_CHART_CNT                                                                   \
    22 /* 曲线渲染点数（0..21：最后一列 21h 与曲线右端对齐，避免折线超出末列） */
#define WEATHER_HOURLY_STEP 30 /* 24h 图每点水平间距（px） */
#define WEATHER_HOURLY_LABEL 3 /* 每 N 小时标注一次温度值 */
#define WEATHER_DAILY_CNT 7    /* 未来 7 天 */
#define WEATHER_DAILY_STEP 84  /* 7 天图每列宽度（px） */

#define WEATHER_FONT_NAME                                                                          \
    (espaperplay_system_get_config()                                                               \
         ->selected_font) /* 当前选用字体（SD 优先，缺则回退 Flash 子集） */

/** 数据是否变化才更新标签（EPD 上避免无谓刷新）。 */
static char s_last_update[32] = "";

/** 快照不可用时的状态（避免秒级轮询重复刷新同一提示文本 / 无谓 EPD 刷新）。 */
static char s_hint[80] = "";
/** 是否已切到快速重试周期（数据就绪后恢复 30s 正常周期）。 */
static bool s_fast_poll = false;
/** 快速重试计数：每 30 次（约 30s）催促一次后台刷新任务（任务刷新失败后会长时间休眠）。 */
static uint32_t s_nudge_cnt = 0;

/* ---- 标题栏（统一状态栏） ---- */
static espaperplay_ui_status_bar_t *s_bar = NULL; /*!< 统一状态栏 */

/* ---- 子页容器与指示点 ---- */
static lv_obj_t *s_subpages[WEATHER_SUBPAGE_CNT];
static lv_obj_t *s_dots[WEATHER_SUBPAGE_CNT];
static int s_page = 0;

/* ---- 子页 0：实时 ---- */
static lv_obj_t *s_temp_label = NULL;  /*!< 温度大字（96px，居中） */
static lv_obj_t *s_unit_label = NULL;  /*!< 单位 °C（右上角） */
static lv_obj_t *s_feel_label = NULL;  /*!< 体感（右侧两行：标签 + 值） */
static lv_obj_t *s_cond_label = NULL;  /*!< 天气 + 今日高低温 */
static lv_obj_t *s_warn_bar = NULL;    /*!< 预警长条（点击弹详情） */
static lv_obj_t *s_warn_detail = NULL; /*!< 预警详情卡片（覆盖层） */
static lv_obj_t *s_warn_title = NULL;
static lv_obj_t *s_warn_desc = NULL;
static lv_obj_t *s_hourly_card = NULL;                                      /*!< 24h 卡片 */
static lv_obj_t *s_hourly_scroll = NULL;                                    /*!< 24h 横向滚动容器 */
static lv_obj_t *s_hourly_chart = NULL;                                     /*!< 24h 温度曲线 */
static lv_obj_t *s_hourly_vals[WEATHER_HOURLY_CNT / WEATHER_HOURLY_LABEL];  /*!< 温度标注 */
static lv_obj_t *s_hourly_icons[WEATHER_HOURLY_CNT / WEATHER_HOURLY_LABEL]; /*!< 图标行 */

/* ---- 子页 1：7 天 + 次要信息 ---- */
static lv_obj_t *s_daily_card = NULL;   /*!< 7 天卡片 */
static lv_obj_t *s_daily_scroll = NULL; /*!< 7 天横向滚动容器 */
static lv_obj_t *s_daily_chart = NULL;  /*!< 高低温双线 */
static lv_obj_t *s_daily_hi[WEATHER_DAILY_CNT] = {NULL};
static lv_obj_t *s_daily_lo[WEATHER_DAILY_CNT] = {NULL};
static lv_obj_t *s_daily_week[WEATHER_DAILY_CNT] = {NULL};
static lv_obj_t *s_daily_icon[WEATHER_DAILY_CNT] = {NULL};
static lv_obj_t *s_info_labels[6] = {NULL}; /*!< 次要信息值 */

/* ---- 子页 2：天文 + 指数 ---- */
static lv_obj_t *s_sun_arc = NULL;
static lv_obj_t *s_moon_arc = NULL;
static lv_obj_t *s_sun_icon = NULL;
static lv_obj_t *s_moon_icon = NULL;
static lv_obj_t *s_index_labels[8] = {NULL};
/* 弧线几何（page2_create 填充）：x, w, sun_base, moon_base, radius */
static int s_arc_geom[5];

/* ---- 页面定时器 / 手势 ---- */
static lv_timer_t *s_weather_timer = NULL;
static bool s_touch_down = false;
static lv_point_t s_touch_start = {0, 0};
static lv_point_t s_touch_last = {0, 0};
/* 按下区域：0=边缘返回，1=图表滚动区（交 LVGL），2=预警条，3=普通区（切页） */
static int s_touch_zone = 3;

/* 前向声明（weather_enter 在 weather_refresh / weather_timer_cb 之前定义）。 */
static void weather_refresh(void);
static void weather_timer_cb(lv_timer_t *timer);

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */

/** FreeType 字体按需加载（与主界面共用缓存字号档）。 */
static lv_font_t *weather_font(int size_px) {
    return espaperplay_fonts_load(WEATHER_FONT_NAME, (uint32_t)size_px,
                                  ESPAPERPLAY_FONT_STYLE_NORMAL);
}

/** 通用标签：白底黑字 + FreeType 字体 + 禁用 LVGL 滚动（防误滑页面）。 */
static lv_obj_t *weather_label_create(lv_obj_t *parent, const char *text, int font_px,
                                      lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_font(label, weather_font(font_px), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

/** 圆角卡片容器（白底黑边框，禁用滚动）。 */
static lv_obj_t *weather_card_create(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/** 逻辑分辨率（旋转后）。 */
static void weather_screen_size(int32_t *out_w, int32_t *out_h) {
    lv_display_t *disp = lv_display_get_default();
    *out_w = lv_display_get_horizontal_resolution(disp);
    *out_h = lv_display_get_vertical_resolution(disp);
}

/** 点在矩形内（逻辑坐标）。 */
static bool weather_point_in(const lv_point_t *p, int x, int y, int w, int h) {
    return p->x >= x && p->x < x + w && p->y >= y && p->y < y + h;
}

/** 对象相对屏幕的坐标（累加父级偏移；LVGL 的 get_x/y 只返回相对父）。 */
static int weather_obj_screen_x(const lv_obj_t *obj) {
    int x = 0;
    const lv_obj_t *p = obj;
    while (p != NULL && lv_obj_get_parent(p) != NULL) {
        x += lv_obj_get_x(p);
        p = lv_obj_get_parent(p);
    }
    return x;
}

static int weather_obj_screen_y(const lv_obj_t *obj) {
    int y = 0;
    const lv_obj_t *p = obj;
    while (p != NULL && lv_obj_get_parent(p) != NULL) {
        y += lv_obj_get_y(p);
        p = lv_obj_get_parent(p);
    }
    return y;
}

/** 解析 "YYYY-MM-DD" 为中文星期（"周一"…"周日"）。 */
static const char *weather_weekday_zh(const char *date) {
    static const char *const wd[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    struct tm tm = {0};
    if (date == NULL || sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) {
        return "--";
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    return wd[tm_local.tm_wday];
}

/** 从完整时间戳截取 "HH:MM"（如 "2026-08-16T05:12+08:00"），并转分钟。 */
static int weather_time_to_min(const char *ts) {
    if (ts == NULL) {
        return -1;
    }
    const char *t = strchr(ts, 'T');
    if (t == NULL) {
        t = ts;
    } else {
        t += 1;
    }
    int h = 0, m = 0;
    if (sscanf(t, "%d:%d", &h, &m) != 2) {
        return -1;
    }
    return h * 60 + m;
}

/** 从完整时间戳截取 "HH:MM"（如 "2026-08-16T05:12+08:00"）；短格式原样返回。 */
static const char *weather_time_hm(const char *ts, char *buf, size_t buf_size) {
    if (ts != NULL && ts[0] != '\0') {
        const char *t = strchr(ts, 'T');
        if (t != NULL && t[1] != '\0' && t[2] != '\0' && t[3] != '\0' && t[4] != '\0') {
            snprintf(buf, buf_size, "%.5s", t + 1);
            return buf;
        }
    }
    return ts != NULL ? ts : "--";
}

/* ------------------------------------------------------------------ */
/* 图表构建                                                             */
/* ------------------------------------------------------------------ */

/** 创建折线图（仅曲线：无网格刻度、不画数据点）。 */
static lv_obj_t *weather_chart_create(lv_obj_t *parent, int w, int h, int point_cnt, int32_t ymin,
                                      int32_t ymax, int series_cnt) {
    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_size(chart, w, h);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, point_cnt);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
    lv_chart_set_div_line_count(chart, 0, 0); /* 无网格刻度 */
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_radius(chart, 0, LV_PART_ITEMS); /* 不画数据点 */
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart, 6, LV_PART_MAIN);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    /* 图表自身不接收按压：手指按在曲线上时触控直接落到外层滚动容器，
     * 保证图表上的横向滑动与外围文本/图标一样可靠地滚动（此前图表
     * 拦截按压时在曲线上滑动偶发无响应）。 */
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < series_cnt; i++) {
        lv_chart_add_series(chart, lv_color_black(), LV_CHART_AXIS_PRIMARY_Y);
    }
    return chart;
}

/** 横向滚动容器（内容超宽时 LVGL 负责滚动）。 */
static lv_obj_t *weather_scroll_create(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *scroll = lv_obj_create(parent);
    lv_obj_set_size(scroll, w, h);
    lv_obj_set_pos(scroll, x, y);
    lv_obj_set_style_bg_color(scroll, lv_color_white(), 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_radius(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_HOR);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    /* 隐藏滚动条提示线、取消滚动到头的回弹 */
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);
    return scroll;
}

/* ------------------------------------------------------------------ */
/* 子页 0：实时                                                          */
/* ------------------------------------------------------------------ */

static void weather_page0_create(lv_obj_t *parent, int w, int h, bool portrait) {
    const int temp_y = portrait ? 40 : 24;
    const int cond_y = portrait ? 200 : 130;
    const int warn_y = portrait ? 252 : 168;
    const int card_y = portrait ? 300 : 206;
    const int card_h = portrait ? 350 : 210;

    /* 温度大字（96px）：文本右对齐；°C 与体感（两行）文本左对齐，
     * 三者组成一个块并整体居中于屏宽（temp_w + 间隔 + side_w），
     * 适配任意分辨率；体感第二行（温度值）底部与温度大字底部对齐 */
    const int temp_w = 170;
    const int side_w = 120;
    const int temp_h = 120; /* 96px 行高估算 */
    const int feel_h = 58;  /* 两行 20px 高 */
    const int block_w = temp_w + 8 + side_w;
    const int x0 = (w - block_w) / 2;
    const int side_x = x0 + temp_w + 8;
    s_temp_label = weather_label_create(parent, "--", 96, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_width(s_temp_label, temp_w);
    lv_obj_set_pos(s_temp_label, x0, temp_y);

    s_unit_label = weather_label_create(parent, "°C", 20, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(s_unit_label, 50);
    lv_obj_set_pos(s_unit_label, side_x, temp_y + 10);

    s_feel_label = weather_label_create(parent, "体感\n--°", 20, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(s_feel_label, side_w);
    lv_obj_set_pos(s_feel_label, side_x, temp_y + temp_h - feel_h);

    /* 天气 + 今日高低温 */
    s_cond_label = weather_label_create(parent, "晴 --℃～--℃", 24, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_cond_label, LV_PCT(100));
    lv_obj_align(s_cond_label, LV_ALIGN_TOP_MID, 0, cond_y);

    /* 预警长条（黑底白字，点击弹详情） */
    s_warn_bar = lv_obj_create(parent);
    lv_obj_set_size(s_warn_bar, w - 2 * WEATHER_MARGIN, 40);
    lv_obj_set_pos(s_warn_bar, WEATHER_MARGIN, warn_y);
    lv_obj_set_style_bg_color(s_warn_bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_warn_bar, 0, 0);
    lv_obj_set_style_radius(s_warn_bar, 8, 0);
    lv_obj_set_style_pad_all(s_warn_bar, 0, 0);
    lv_obj_remove_flag(s_warn_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *warn_text = lv_label_create(s_warn_bar);
    lv_label_set_text(warn_text, "⚠ 预警");
    lv_obj_set_style_text_color(warn_text, lv_color_white(), 0);
    lv_obj_set_style_text_font(warn_text, weather_font(16), 0);
    lv_obj_center(warn_text);
    lv_obj_set_user_data(s_warn_bar, warn_text);

    /* 24h 卡片：标题 + 滚动区（温度标注行 + 曲线 + 图标行） */
    s_hourly_card =
        weather_card_create(parent, WEATHER_MARGIN, card_y, w - 2 * WEATHER_MARGIN, card_h);

    lv_obj_t *title =
        weather_label_create(s_hourly_card, "未来 24 小时气温", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_pos(title, 4, 2);

    const int scroll_y = 30; /* 标题与内容间隔 */
    const int scroll_w = w - 2 * WEATHER_MARGIN - 16;
    lv_obj_t *scroll =
        weather_scroll_create(s_hourly_card, 0, scroll_y, scroll_w, card_h - scroll_y - 8);
    s_hourly_scroll = scroll;
    /* 容器左内边距：仅防第一个标注被裁剪，图表起点尽量靠左 */
    lv_obj_set_style_pad_left(scroll, 18, 0);
    /* 图表宽 = 曲线渲染点数步进 + 两侧 pad：点间距严格等于 WEATHER_HOURLY_STEP，
     * 曲线渲染到 21h（= 最后一列标注），右端与末列对齐 */
    const int chart_w = (WEATHER_HOURLY_CHART_CNT - 1) * WEATHER_HOURLY_STEP + 12;
    const int val_h = 22; /* 标注行高 */
    const int scroll_h = card_h - scroll_y - 8;
    const int icon_y = scroll_h - 52;       /* 图标行固定，距滚动区底 20px（防贴框） */
    const int chart_h = icon_y - val_h - 8; /* 曲线高 */

    /* 温度标注行（每 3 小时一个值，中心对齐曲线点；点 0 内容 x = 12） */
    for (int i = 0; i < WEATHER_HOURLY_CNT / WEATHER_HOURLY_LABEL; i++) {
        const int x = i * WEATHER_HOURLY_LABEL * WEATHER_HOURLY_STEP - 18;
        lv_obj_t *v = weather_label_create(scroll, "--", 16, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(v, 60);
        lv_obj_set_pos(v, x, 0);
        s_hourly_vals[i] = v;

        lv_obj_t *img = lv_image_create(scroll);
        lv_obj_set_pos(img, x + 14, icon_y);
        s_hourly_icons[i] = img;
    }

    s_hourly_chart =
        weather_chart_create(scroll, chart_w, chart_h, WEATHER_HOURLY_CHART_CNT, 0, 40, 1);
    lv_obj_set_pos(s_hourly_chart, 6, val_h);
}

/* ------------------------------------------------------------------ */
/* 子页 1：7 天 + 次要信息                                                */
/* ------------------------------------------------------------------ */

static void weather_page1_create(lv_obj_t *parent, int w, int h, bool portrait) {
    const int card_h = portrait ? 320 : 240;
    const int card_y = portrait ? 40 : 30;
    s_daily_card =
        weather_card_create(parent, WEATHER_MARGIN, card_y, w - 2 * WEATHER_MARGIN, card_h);

    lv_obj_t *title = weather_label_create(s_daily_card, "未来 7 天预报", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_pos(title, 4, 2);

    const int scroll_y = 30; /* 标题与内容间隔 */
    const int scroll_w = w - 2 * WEATHER_MARGIN - 16;
    lv_obj_t *scroll =
        weather_scroll_create(s_daily_card, 0, scroll_y, scroll_w, card_h - scroll_y - 8);
    s_daily_scroll = scroll;
    /* 容器左内边距：仅防第一列被裁剪，图表起点尽量靠左 */
    lv_obj_set_style_pad_left(scroll, 18, 0);
    /* 图表宽 = 列距 * (n-1) + 两侧 pad：点间距严格等于 WEATHER_DAILY_STEP */
    const int chart_w = (WEATHER_DAILY_CNT - 1) * WEATHER_DAILY_STEP + 12;
    const int col_w = 60;                                /* 列文字宽度（文本居中于列） */
    const int hi_y = 0;                                  /* 最高温标注行 */
    const int week_y = 24;                               /* 代称行 */
    const int icon_y = 48;                               /* 图标行（32px） */
    const int chart_y = 84;                              /* 曲线 y */
    const int chart_h = card_h - scroll_y - 8 - 84 - 38; /* 曲线高（下方留最低温行） */
    const int lo_y = chart_y + chart_h + 6;              /* 最低温标注行 */

    s_daily_chart = weather_chart_create(scroll, chart_w, chart_h, WEATHER_DAILY_CNT, -10, 40, 2);
    lv_obj_set_pos(s_daily_chart, 6, chart_y);

    for (int i = 0; i < WEATHER_DAILY_CNT; i++) {
        /* 列中心对齐图表点（点 i 内容 x = 12 + i*84） */
        const int cx = 12 + i * WEATHER_DAILY_STEP;
        lv_obj_t *hi = weather_label_create(scroll, "--", 20, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(hi, col_w);
        lv_obj_set_pos(hi, cx - col_w / 2, hi_y);
        lv_obj_t *wk = weather_label_create(scroll, "--", 16, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(wk, col_w);
        lv_obj_set_pos(wk, cx - col_w / 2, week_y);
        lv_obj_t *ic = lv_image_create(scroll);
        lv_obj_set_pos(ic, cx - 16, icon_y);
        lv_obj_t *lo = weather_label_create(scroll, "--", 20, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(lo, col_w);
        lv_obj_set_pos(lo, cx - col_w / 2, lo_y);
        s_daily_hi[i] = hi;
        s_daily_week[i] = wk;
        s_daily_icon[i] = ic;
        s_daily_lo[i] = lo;
    }

    /* 次要信息：湿度 / 风 / 降水 / 气压 / 能见度 / 体感（卡片网格）。
     * 列数按剩余高度约束（避免横屏两行超出容器底部）：
     * 两行放得下 -> 竖屏 2 列 x3 行 / 横屏 3 列 x2 行；否则单行 6 列。 */
    const int margin = WEATHER_MARGIN;
    const int gap = 12;
    const int info_y = card_y + card_h + 14;
    const int avail_info_h = h - info_y; /* 子页容器内剩余高度 */
    /* 竖屏 2 列需 3 行、横屏 3 列需 2 行；放不下则压缩为单行 6 列。 */
    const int need_info_h = portrait ? 3 * 66 + 2 * gap : 2 * 66 + gap;
    int cols = (avail_info_h >= need_info_h) ? (portrait ? 2 : 3) : 6;
    const int cw = (w - 2 * margin - (cols - 1) * gap) / cols;
    const int ch = 66;
    static const struct {
        const lv_image_dsc_t *icon;
        const char *name;
    } infos[6] = {
        {&icon_humidity_16, "湿度"}, {&icon_wind_16, "风"},           {&icon_rain_16, "降水"},
        {&icon_pressure_16, "气压"}, {&icon_visibility_16, "能见度"}, {&icon_thermo_16, "体感"},
    };
    for (int i = 0; i < 6; i++) {
        const int r = i / cols;
        const int c = i % cols;
        lv_obj_t *card =
            weather_card_create(parent, margin + c * (cw + gap), info_y + r * (ch + gap), cw, ch);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_t *ic = lv_image_create(card);
        lv_image_set_src(ic, infos[i].icon);
        lv_obj_set_pos(ic, 6, 3);
        lv_obj_t *n = weather_label_create(card, infos[i].name, 16, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(n, cw - 30);
        lv_obj_set_pos(n, 28, 2);
        lv_obj_t *v = weather_label_create(card, "--", 20, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(v, cw - 12);
        lv_obj_set_pos(v, 6, 22);
        s_info_labels[i] = v;
    }
}

/* ------------------------------------------------------------------ */
/* 子页 2：天文 + 指数                                                    */
/* ------------------------------------------------------------------ */

static void weather_page2_create(lv_obj_t *parent, int w, int h, bool portrait) {
    const int margin = WEATHER_MARGIN;
    const int arc_h = portrait ? 300 : 210;
    const int arc_y = portrait ? 40 : 30;

    lv_obj_t *card = weather_card_create(parent, margin, arc_y, w - 2 * margin, arc_h);

    lv_obj_t *sun_l = weather_label_create(card, "日出 --", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(sun_l, 90);
    lv_obj_set_pos(sun_l, 6, 4);
    lv_obj_t *sun_r = weather_label_create(card, "日落 --", 16, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_width(sun_r, 90);
    lv_obj_align(sun_r, LV_ALIGN_TOP_RIGHT, -6, 4);

    lv_obj_t *moon_l = weather_label_create(card, "月出 --", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(moon_l, 90);
    lv_obj_set_pos(moon_l, 6, 8 + arc_h / 2);
    lv_obj_t *moon_r = weather_label_create(card, "月落 --", 16, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_width(moon_r, 90);
    lv_obj_align(moon_r, LV_ALIGN_TOP_RIGHT, -6, 8 + arc_h / 2);

    /* 太阳弧（上）与月亮弧（下） */
    const int arc_w = w - 2 * margin - 200;
    const int arc_x = margin + 100;
    const int sun_base = 40 + (arc_h / 2 - 40) / 2;
    const int moon_base = arc_h / 2 + 44 + (arc_h / 2 - 44) / 2;
    const int radius = (arc_h / 2 - 44) / 2;

    s_sun_arc = lv_line_create(card);
    s_moon_arc = lv_line_create(card);
    lv_obj_set_style_line_color(s_sun_arc, lv_color_black(), 0);
    lv_obj_set_style_line_color(s_moon_arc, lv_color_black(), 0);
    lv_obj_set_style_line_width(s_sun_arc, 1, 0);
    lv_obj_set_style_line_width(s_moon_arc, 1, 0);

    static lv_point_precise_t sun_pts[25], moon_pts[25];
    for (int i = 0; i <= 24; i++) {
        const float t = (float)i / 24.0f;
        const float ang = (float)M_PI * t;
        const int x = arc_x + (int)lroundf((float)arc_w * t);
        sun_pts[i].x = x;
        sun_pts[i].y = sun_base - (int)lroundf((float)radius * sinf(ang));
        moon_pts[i].x = x;
        moon_pts[i].y = moon_base - (int)lroundf((float)radius * sinf(ang));
    }
    lv_line_set_points(s_sun_arc, sun_pts, 25);
    lv_line_set_points(s_moon_arc, moon_pts, 25);

    s_sun_icon = lv_image_create(card);
    s_moon_icon = lv_image_create(card);
    const lv_image_dsc_t *sun_ic = qweather_icon_get_small("100");
    const lv_image_dsc_t *moon_ic = qweather_icon_get_small("150");
    if (sun_ic != NULL) {
        lv_image_set_src(s_sun_icon, sun_ic);
    }
    if (moon_ic != NULL) {
        lv_image_set_src(s_moon_icon, moon_ic);
    }
    s_arc_geom[0] = arc_x;
    s_arc_geom[1] = arc_w;
    s_arc_geom[2] = sun_base;
    s_arc_geom[3] = moon_base;
    s_arc_geom[4] = radius;

    /* 指数：洗车等（卡片网格）。列数按剩余高度约束（同次要信息网格）：
     * 横屏 3 列需 3 行会超出容器底部，放不下两行时压缩为 4 列两行。 */
    const int gap = 12;
    const int idx_y = arc_y + arc_h + 14;
    const int avail_idx_h = h - idx_y;
    const int rows_fit = avail_idx_h / (56 + gap); /* 可完整容纳的行数 */
    /* 2 列恒需 4 行（竖/横皆同）；放得下 2 行则 4 列，否则 8 列单行。 */
    const int cols = (rows_fit >= 4) ? 2 : ((rows_fit >= 2) ? 4 : 8);
    const int cw = (w - 2 * margin - (cols - 1) * gap) / cols;
    const int ch = 56;
    for (int i = 0; i < 8; i++) {
        const int r = i / cols;
        const int c = i % cols;
        lv_obj_t *icard =
            weather_card_create(parent, margin + c * (cw + gap), idx_y + r * (ch + gap), cw, ch);
        lv_obj_t *label = weather_label_create(icard, "--", 16, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_center(label);
        s_index_labels[i] = label;
    }
}

/* ------------------------------------------------------------------ */
/* 页面构建 / 子页切换                                                   */
/* ------------------------------------------------------------------ */

static void weather_show_page(int idx) {
    if (idx < 0 || idx >= WEATHER_SUBPAGE_CNT || idx == s_page) {
        return;
    }
    s_page = idx;
    for (int i = 0; i < WEATHER_SUBPAGE_CNT; i++) {
        if (i == idx) {
            lv_obj_remove_flag(s_subpages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_subpages[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(s_dots[i], i == idx ? lv_color_black() : lv_color_white(), 0);
    }
    ESP_LOGI(TAG, "weather: subpage %d", idx);
}

static void weather_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    /* 防止整个屏幕被 LVGL 滚动（页面滚动由手势切换子页接管） */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t scr_w, scr_h;
    weather_screen_size(&scr_w, &scr_h);
    const bool portrait = scr_w < scr_h;

    /* 统一状态栏：左侧时间、居中位置名（动态）、右侧 WiFi/睡眠图标 */
    s_bar = espaperplay_ui_status_bar_create(scr, WEATHER_BAR_H_PX, NULL, false);

    /* 子页容器 */
    const int area_y = WEATHER_BAR_H_PX;
    const int area_h = scr_h - area_y - 24;
    for (int i = 0; i < WEATHER_SUBPAGE_CNT; i++) {
        s_subpages[i] = lv_obj_create(scr);
        lv_obj_set_size(s_subpages[i], scr_w, area_h);
        lv_obj_set_pos(s_subpages[i], 0, area_y);
        lv_obj_set_style_bg_color(s_subpages[i], lv_color_white(), 0);
        lv_obj_set_style_border_width(s_subpages[i], 0, 0);
        lv_obj_set_style_pad_all(s_subpages[i], 0, 0);
        lv_obj_remove_flag(s_subpages[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    weather_page0_create(s_subpages[0], scr_w, area_h, portrait);
    weather_page1_create(s_subpages[1], scr_w, area_h, portrait);
    weather_page2_create(s_subpages[2], scr_w, area_h, portrait);

    /* 指示点 */
    for (int i = 0; i < WEATHER_SUBPAGE_CNT; i++) {
        s_dots[i] = lv_obj_create(scr);
        lv_obj_set_size(s_dots[i], 10, 10);
        lv_obj_set_pos(s_dots[i], scr_w / 2 + (i - (WEATHER_SUBPAGE_CNT - 1) / 2) * 24 - 5,
                       scr_h - 18);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_dots[i], 1, 0);
        lv_obj_set_style_border_color(s_dots[i], lv_color_black(), 0);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    s_page = 0;
    for (int i = 0; i < WEATHER_SUBPAGE_CNT; i++) {
        if (i != 0) {
            lv_obj_add_flag(s_subpages[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(s_dots[i], i == 0 ? lv_color_black() : lv_color_white(), 0);
    }

    /* 预警详情覆盖卡（初始隐藏） */
    s_warn_detail =
        weather_card_create(scr, WEATHER_MARGIN, area_y + 60, scr_w - 2 * WEATHER_MARGIN, 340);
    lv_obj_add_flag(s_warn_detail, LV_OBJ_FLAG_HIDDEN);
    s_warn_title = weather_label_create(s_warn_detail, "", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(s_warn_title, LV_PCT(100));
    lv_obj_set_pos(s_warn_title, 4, 4);
    s_warn_desc = weather_label_create(s_warn_detail, "", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(s_warn_desc, LV_PCT(100));
    lv_obj_set_pos(s_warn_desc, 4, 40);

    s_last_update[0] = '\0';
    s_hint[0] = '\0';
    s_fast_poll = false;
    s_nudge_cnt = 0;
    s_touch_down = false;
    s_touch_zone = 3;

    s_weather_timer = lv_timer_create(weather_timer_cb, WEATHER_UI_PERIOD_MS, NULL);
    if (s_weather_timer == NULL) {
        /* 定时器创建失败：只有进入时的一次刷新，无法周期更新（罕见，仅记录）。 */
        ESP_LOGE(TAG, "weather: periodic refresh timer create failed");
    }
    weather_refresh();

    ESP_LOGI(TAG, "weather screen entered");
}

/** 天气页退出（页面 exit：删除定时器，避免离开页面后仍刷新屏幕）。 */
static void weather_exit(void) {
    if (s_weather_timer != NULL) {
        lv_timer_delete(s_weather_timer);
        s_weather_timer = NULL;
    }
    ESP_LOGI(TAG, "weather screen exited");
}

/* ------------------------------------------------------------------ */
/* 内容刷新                                                             */
/* ------------------------------------------------------------------ */

static void weather_refresh(void) {
    /* 快照较大（约 20KB），在堆上分配。 */
    espaperplay_weather_snapshot_t *snap = malloc(sizeof(*snap));
    if (snap == NULL) {
        ESP_LOGW(TAG, "weather: snapshot alloc failed");
        return;
    }
    if (espaperplay_weather_get_snapshot(snap) != ESP_OK || !snap->valid) {
        /* 快照尚不可用（启动初期数据未就绪 / 刷新失败）：给出准确提示，并把
         * 本轮询切成秒级快速重试——天气获取成功后页面即刻更新，无需等下一
         * 个 30s 周期。同时周期性催促后台刷新任务（某次整体刷新失败后任务会
         * 长时间休眠直到下一个周期，仅靠页面轮询会一直等不到数据）。 */
        espaperplay_weather_status_t st;
        const bool configured =
            (espaperplay_weather_get_status(&st) == ESP_OK) ? st.configured : false;
        const char *hint = configured ? "正在获取天气数据…" : "未配置 API Key，请在 Web 管理页设置";
        if (strcmp(s_hint, hint) != 0) {
            strlcpy(s_hint, hint, sizeof(s_hint));
            espaperplay_ui_status_bar_set_title(s_bar, "--");
            lv_label_set_text(s_temp_label, "--");
            lv_label_set_text(s_cond_label, hint);
        }
        if (s_nudge_cnt % 30 == 0) { /* 每 ~30s 催促一次（进页即催促一次） */
            espaperplay_weather_request_refresh();
        }
        s_nudge_cnt++;
        if (s_weather_timer != NULL && !s_fast_poll) {
            lv_timer_set_period(s_weather_timer, WEATHER_UI_RETRY_MS);
            s_fast_poll = true;
        }
        free(snap);
        return;
    }

    /* 数据可用：恢复正常 30s 刷新周期，并清空"获取中"状态。 */
    if (s_fast_poll && s_weather_timer != NULL) {
        lv_timer_set_period(s_weather_timer, WEATHER_UI_PERIOD_MS);
        s_fast_poll = false;
    }
    s_nudge_cnt = 0;
    s_hint[0] = '\0';

    /* 更新时变化才刷新标签（EPD 局部刷新省电）。 */
    if (strcmp(s_last_update, snap->update_time) == 0) {
        free(snap);
        return;
    }
    strlcpy(s_last_update, snap->update_time, sizeof(s_last_update));

    const espaperplay_weather_now_t *now = &snap->now;
    char buf[256];

    /* 标题栏：位置名（动态，经统一状态栏标题更新）。 */
    espaperplay_ui_status_bar_set_title(s_bar, snap->location_name);

    /* 温度大字 + 体感（两行） */
    lv_label_set_text(s_temp_label, now->temp[0] ? now->temp : "--");
    snprintf(buf, sizeof(buf), "体感\n%s°", now->feels_like[0] ? now->feels_like : "--");
    lv_label_set_text(s_feel_label, buf);

    /* 天气 + 今日高低温 */
    if (snap->daily_count > 0) {
        snprintf(buf, sizeof(buf), "%s  %s℃～%s℃", now->text[0] ? now->text : "--",
                 snap->daily[0].temp_min[0] ? snap->daily[0].temp_min : "--",
                 snap->daily[0].temp_max[0] ? snap->daily[0].temp_max : "--");
    } else {
        snprintf(buf, sizeof(buf), "%s", now->text[0] ? now->text : "--");
    }
    lv_label_set_text(s_cond_label, buf);

    /* 预警长条 + 详情内容 */
    if (snap->warning_count > 0) {
        const espaperplay_weather_warning_t *w = &snap->warnings[0];
        lv_obj_t *wt = lv_obj_get_user_data(s_warn_bar);
        if (wt != NULL) {
            snprintf(buf, sizeof(buf), "⚠ %s%s预警", w->type_name[0] ? w->type_name : "",
                     w->level[0] ? w->level : "");
            lv_label_set_text(wt, buf);
        }
        lv_obj_remove_flag(s_warn_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_warn_title, w->title[0] ? w->title : "预警");
        lv_label_set_text(s_warn_desc, w->text[0] ? w->text : "");
    } else {
        lv_obj_add_flag(s_warn_bar, LV_OBJ_FLAG_HIDDEN);
    }

    /* 24h 曲线 + 温度标注 + 图标行 */
    if (snap->hourly_count > 0) {
        static int32_t raw[WEATHER_HOURLY_CNT];
        /* 曲线只渲染到 21h（与末列标注对齐）；数据仍按最多 24 点读取。 */
        int n = snap->hourly_count > WEATHER_HOURLY_CHART_CNT ? WEATHER_HOURLY_CHART_CNT
                                                              : snap->hourly_count;
        int32_t ymin = 100, ymax = -100;
        for (int i = 0; i < n; i++) {
            raw[i] = atoi(snap->hourly[i].temp);
            if (raw[i] < ymin)
                ymin = raw[i];
            if (raw[i] > ymax)
                ymax = raw[i];
        }
        lv_chart_set_point_count(s_hourly_chart, n);
        /* 范围以最高/最低温度为准 */
        lv_chart_set_range(s_hourly_chart, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
        lv_chart_series_t *ser = lv_chart_get_series_next(s_hourly_chart, NULL);
        lv_chart_set_series_values(s_hourly_chart, ser, raw, (size_t)n);

        for (int i = 0; i < WEATHER_HOURLY_CNT / WEATHER_HOURLY_LABEL; i++) {
            const int idx = i * WEATHER_HOURLY_LABEL;
            if (idx < n) {
                snprintf(buf, sizeof(buf), "%s°", snap->hourly[idx].temp);
                lv_label_set_text(s_hourly_vals[i], buf);
                const lv_image_dsc_t *ic = qweather_icon_get_small(snap->hourly[idx].icon);
                if (ic != NULL) {
                    lv_image_set_src(s_hourly_icons[i], ic);
                }
            }
        }
        lv_chart_refresh(s_hourly_chart);
    }

    /* 7 天双曲线 + 高/低温标注 + 代称/图标 */
    if (snap->daily_count > 0) {
        static int32_t raw_hi[WEATHER_DAILY_CNT], raw_lo[WEATHER_DAILY_CNT];
        int n = snap->daily_count > WEATHER_DAILY_CNT ? WEATHER_DAILY_CNT : snap->daily_count;
        int32_t ymin = 100, ymax = -100;
        for (int i = 0; i < n; i++) {
            raw_hi[i] = atoi(snap->daily[i].temp_max);
            raw_lo[i] = atoi(snap->daily[i].temp_min);
            if (raw_hi[i] > ymax)
                ymax = raw_hi[i];
            if (raw_lo[i] < ymin)
                ymin = raw_lo[i];
        }
        lv_chart_set_point_count(s_daily_chart, n);
        /* 范围以最高的最高气温 / 最低的最低气温为准 */
        lv_chart_set_range(s_daily_chart, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
        lv_chart_series_t *hi = lv_chart_get_series_next(s_daily_chart, NULL);
        lv_chart_series_t *lo = lv_chart_get_series_next(s_daily_chart, hi);
        lv_chart_set_series_values(s_daily_chart, hi, raw_hi, (size_t)n);
        lv_chart_set_series_values(s_daily_chart, lo, raw_lo, (size_t)n);

        for (int i = 0; i < WEATHER_DAILY_CNT; i++) {
            if (i >= n) {
                continue;
            }
            const espaperplay_weather_daily_t *d = &snap->daily[i];
            const char *wd = weather_weekday_zh(d->fx_date);
            if (i == 0) {
                snprintf(buf, sizeof(buf), "今天");
            } else if (i == 1) {
                snprintf(buf, sizeof(buf), "明天");
            } else {
                snprintf(buf, sizeof(buf), "%s", wd);
            }
            lv_label_set_text(s_daily_week[i], buf);
            const lv_image_dsc_t *ic = qweather_icon_get_small(d->icon_day);
            if (ic != NULL) {
                lv_image_set_src(s_daily_icon[i], ic);
            }
            snprintf(buf, sizeof(buf), "%s°", d->temp_max[0] ? d->temp_max : "-");
            lv_label_set_text(s_daily_hi[i], buf);
            snprintf(buf, sizeof(buf), "%s°", d->temp_min[0] ? d->temp_min : "-");
            lv_label_set_text(s_daily_lo[i], buf);
        }
        lv_chart_refresh(s_daily_chart);
    }

    /* 次要信息：湿度 / 风 / 降水 / 气压 / 能见度 / 体感 */
    {
        char vals[6][40];
        snprintf(vals[0], sizeof(vals[0]), "%s%%", now->humidity[0] ? now->humidity : "--");
        snprintf(vals[1], sizeof(vals[1]), "%s%s级", now->wind_dir[0] ? now->wind_dir : "--",
                 now->wind_scale[0] ? now->wind_scale : "-");
        snprintf(vals[2], sizeof(vals[2]), "%s mm", now->precip[0] ? now->precip : "--");
        snprintf(vals[3], sizeof(vals[3]), "%s hPa", now->pressure[0] ? now->pressure : "--");
        snprintf(vals[4], sizeof(vals[4]), "%s km", now->vis[0] ? now->vis : "--");
        snprintf(vals[5], sizeof(vals[5]), "%s°", now->feels_like[0] ? now->feels_like : "--");
        for (int i = 0; i < 6; i++) {
            if (s_info_labels[i] != NULL) {
                lv_label_set_text(s_info_labels[i], vals[i]);
            }
        }
    }

    /* 日出日落 / 月出月落：时间 + 弧线图标定位 */
    {
        const int sr = weather_time_to_min(snap->astronomy.sunrise);
        const int ss = weather_time_to_min(snap->astronomy.sunset);
        const int mr = weather_time_to_min(snap->astronomy.moonrise);
        const int ms = weather_time_to_min(snap->astronomy.moonset);

        lv_obj_t *card = lv_obj_get_parent(s_sun_arc);
        lv_obj_t *sun_l = lv_obj_get_child(card, 0);
        lv_obj_t *sun_r = lv_obj_get_child(card, 1);
        lv_obj_t *moon_l = lv_obj_get_child(card, 2);
        lv_obj_t *moon_r = lv_obj_get_child(card, 3);
        if (sun_l && sun_r && moon_l && moon_r) {
            char t1[8], t2[8], t3[8], t4[8];
            char b1[64], b2[64], b3[64], b4[64];
            snprintf(b1, sizeof(b1), "日出 %s",
                     weather_time_hm(snap->astronomy.sunrise, t1, sizeof(t1)));
            snprintf(b2, sizeof(b2), "日落 %s",
                     weather_time_hm(snap->astronomy.sunset, t2, sizeof(t2)));
            snprintf(b3, sizeof(b3), "月出 %s",
                     weather_time_hm(snap->astronomy.moonrise, t3, sizeof(t3)));
            snprintf(b4, sizeof(b4), "月落 %s",
                     weather_time_hm(snap->astronomy.moonset, t4, sizeof(t4)));
            lv_label_set_text(sun_l, b1);
            lv_label_set_text(sun_r, b2);
            lv_label_set_text(moon_l, b3);
            lv_label_set_text(moon_r, b4);
        }

        /* 当前时间定位太阳/月亮在弧上的位置：
         * 时间比例 = (now - 出) / (落 - 出)，支持跨午夜（落 < 出 时 +1440 调整）；
         * 当前时间不在 [出, 落] 区间内则隐藏图标（已日落/月落不再显示）。 */
        struct tm now_tm;
        const bool have_time = (espaperplay_clock_get_local_time(&now_tm) == ESP_OK);
        if (have_time && sr >= 0 && ss > sr) {
            const int now_min = now_tm.tm_hour * 60 + now_tm.tm_min;
            float t = (float)(now_min - sr) / (float)(ss - sr);
            if (t >= 0.0f && t <= 1.0f) {
                const int x = s_arc_geom[0] + (int)lroundf((float)s_arc_geom[1] * t);
                const int y =
                    s_arc_geom[2] - (int)lroundf((float)s_arc_geom[4] * sinf((float)M_PI * t));
                /* 图标 32x32：中心对齐曲线点 */
                lv_obj_set_pos(s_sun_icon, x - 16, y - 16);
                lv_obj_remove_flag(s_sun_icon, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_sun_icon, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(s_sun_icon, LV_OBJ_FLAG_HIDDEN);
        }
        if (have_time && mr >= 0) {
            /* 跨午夜：月落早于月出时按次日处理 */
            int ms_adj = ms;
            if (ms_adj < mr) {
                ms_adj += 1440;
            }
            int now_min = now_tm.tm_hour * 60 + now_tm.tm_min;
            if (now_min < mr && ms_adj > mr) {
                now_min += 1440; /* 午夜后、月出前的时间段属"前一日弧段" */
            }
            const float t = (float)(now_min - mr) / (float)(ms_adj - mr);
            if (t >= 0.0f && t <= 1.0f) {
                const int x = s_arc_geom[0] + (int)lroundf((float)s_arc_geom[1] * t);
                const int y =
                    s_arc_geom[3] - (int)lroundf((float)s_arc_geom[4] * sinf((float)M_PI * t));
                lv_obj_set_pos(s_moon_icon, x - 16, y - 16);
                lv_obj_remove_flag(s_moon_icon, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_moon_icon, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(s_moon_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 指数：前 8 条（含洗车等） */
    for (int i = 0; i < 8; i++) {
        if (i < snap->indices_count) {
            const espaperplay_weather_indices_t *in = &snap->indices[i];
            snprintf(buf, sizeof(buf), "%s %s", in->name[0] ? in->name : "--",
                     in->level[0] ? in->level : "");
            lv_label_set_text(s_index_labels[i], buf);
        } else if (s_index_labels[i] != NULL) {
            lv_label_set_text(s_index_labels[i], "");
        }
    }

    ESP_LOGI(TAG, "weather screen refreshed: %s %s°C, alerts %d", snap->location_name, now->temp,
             snap->warning_count);
    free(snap);
}

/** 周期刷新（LVGL 线程内，lv_timer 驱动）。 */
static void weather_timer_cb(lv_timer_t *timer) {
    (void)timer;
    weather_refresh();
}

/* ------------------------------------------------------------------ */
/* 返回手势 / 按键                                                      */
/* ------------------------------------------------------------------ */

/** 天气页触摸处理：边缘向内滑动返回；中间横向滑动切换子页；
 * 图表滚动区内的滑动交给 LVGL 滚动；点击预警条弹详情。 */
static void weather_on_touch(const espaperplay_input_event_t *event) {
    lv_point_t p;
    espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

    if (event->touch_pressed) {
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start = p;

            int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());

            /* 预警详情卡打开时：点击任意处关闭 */
            if (s_warn_detail != NULL && !lv_obj_has_flag(s_warn_detail, LV_OBJ_FLAG_HIDDEN)) {
                s_touch_zone = 4;
                return;
            }

            /* 图表滚动区优先判定（含边缘区域：在图表上滑动应滚图表，
             * 而不是返回主页或切换子页） */
            if (s_page == 0 && s_hourly_scroll != NULL &&
                weather_point_in(&p, weather_obj_screen_x(s_hourly_scroll),
                                 weather_obj_screen_y(s_hourly_scroll),
                                 lv_obj_get_width(s_hourly_scroll),
                                 lv_obj_get_height(s_hourly_scroll))) {
                s_touch_zone = 1; /* 24h 图表滚动区（仅子页 0） */
            } else if (s_page == 1 && s_daily_scroll != NULL &&
                       weather_point_in(&p, weather_obj_screen_x(s_daily_scroll),
                                        weather_obj_screen_y(s_daily_scroll),
                                        lv_obj_get_width(s_daily_scroll),
                                        lv_obj_get_height(s_daily_scroll))) {
                s_touch_zone = 1; /* 7 天图表滚动区（仅子页 1） */
            } else if (s_page == 0 && !lv_obj_has_flag(s_warn_bar, LV_OBJ_FLAG_HIDDEN) &&
                       weather_point_in(
                           &p, weather_obj_screen_x(s_warn_bar), weather_obj_screen_y(s_warn_bar),
                           lv_obj_get_width(s_warn_bar), lv_obj_get_height(s_warn_bar))) {
                s_touch_zone = 2; /* 预警条（仅子页 0） */
            } else if (p.x < WEATHER_EDGE_PX || p.x > scr_w - WEATHER_EDGE_PX) {
                s_touch_zone = 0; /* 边缘返回 */
            } else {
                s_touch_zone = 3; /* 普通区：切换子页 */
            }
        }
        s_touch_last = p;
    } else if (s_touch_down) {
        s_touch_down = false;
        const int dx = s_touch_last.x - s_touch_start.x;
        const int dy = s_touch_last.y - s_touch_start.y;
        const int adx = abs(dx);
        const int ady = abs(dy);

        if (s_touch_zone == 4) {
            lv_obj_add_flag(s_warn_detail, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        if (s_touch_zone == 0) {
            int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
            if ((s_touch_start.x < WEATHER_EDGE_PX && dx > WEATHER_EDGE_SWIPE_PX) ||
                (s_touch_start.x > scr_w - WEATHER_EDGE_PX && dx < -WEATHER_EDGE_SWIPE_PX)) {
                if (espaperplay_ui_page_depth() > 1) {
                    ESP_LOGI(TAG, "weather: edge swipe -> pop back");
                    espaperplay_ui_page_pop_lv();
                }
            }
            return;
        }
        if (s_touch_zone == 2) {
            if (adx <= 15 && ady <= 15) {
                lv_obj_remove_flag(s_warn_detail, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }
        if (s_touch_zone == 1) {
            return; /* 图表滚动区：交给 LVGL 滚动 */
        }
        if (adx > WEATHER_SWIPE_PX && adx > ady * 1.2f) {
            weather_show_page(s_page + (dx < 0 ? 1 : -1));
        }
    }
}

/** 天气页按键处理（LVGL 线程内）：单击返回上一页。 */
static void weather_on_key(const espaperplay_input_event_t *event) {
    if (event->key_action == ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK &&
        espaperplay_ui_page_depth() > 1) {
        ESP_LOGI(TAG, "weather: single click -> pop back");
        espaperplay_ui_page_pop_lv();
    }
}

/** 天气页页面实例（页面栈用）。 */
const espaperplay_ui_page_t espaperplay_ui_page_weather = {weather_enter, weather_exit,
                                                           weather_on_key, weather_on_touch};
