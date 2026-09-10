/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "espaperplay_clock.h"
#include "espaperplay_gui.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_hitokoto.h"
#include "espaperplay_input.h"
#include "espaperplay_power.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_util.h"
#include "espaperplay_weather.h"
#include "espaperplay_wifi.h"

#include "icons_data.h"
#include "qweather_icons.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 睡眠屏保页（桌面时钟版式）
 * ====================================================================
 *
 * 触发：自动浅睡眠管理任务判定入睡且栈顶为主界面时，经
 * espaperplay_ui_screensaver_show() 把本页压入页面栈顶并同步渲染落屏
 * （lv_refr_now + wait_idle），随后设备才真正进入浅睡眠——电子纸的
 * 双稳态特性使屏保在断刷新的睡眠期间持续显示。
 *
 * 恢复：用户唤醒（触摸/按键/串口）由电源管理调用
 * espaperplay_ui_screensaver_dismiss() 弹出本页、重建主界面；本页自身的
 * on_touch / on_key 也会自退出（覆盖「压屏保后、入睡前的窗口内用户
 * 操作」与幻影触摸帧等时序，两条路径幂等互兜底）。定时器唤醒不清屏保：
 * 借 3s 刷新窗口由本页 1s 定时器更新时钟后重新入睡。
 *
 * 版式（桌面时钟；竖屏单列自上而下，横屏左列时间/天气/一言 + 右列月历）：
 *   - 头部行：公历日期 + 农历（clock 服务本地换算，覆盖 2020..2077）
 *     左对齐，右侧 WiFi 信号图标；
 *   - 时间盒：HH:MM 大字 + 星期，圆角边框盒；
 *   - 天气盒：单行仪表布局——天气图标 + 天况 + 温/湿[/风]指标（宽盒含
 *     风向风级，横屏窄盒只放温/湿）；
 *   - 月历盒：年月标题 + 星期表头（周末反色小块）+ 6x7 网格，
 *     今日反白圆标（黑白屏以反色表达参考稿的红色强调）；
 *   - 一言盒：Hitokoto 句子（服务快照，小时级刷新）+ 出处作者行；
 *     句子尚未取到时按时段回退问候语；
 *   - 页脚：本机 IP 左、版本号右（Web 管理页入口提示）。
 * 盒高按剩余空间比例分配并钳制（极端矮屏日历行距收紧，以 480x800
 * 竖屏为设计基准）；文本 dedup 刷新（内容未变不触发 EPD 局刷），
 * 月历仅在月份或今日变化时触碰网格。
 */

#define SAV_UI_PERIOD_MS 1000   /* 内容轮询周期（dedup：内容未变不刷新 EPD） */
#define SAV_SYNCED_YEAR 2024    /* 判定 NTP 已同步的最小年份 */
#define SAV_LV_CALL_MS 4000     /* 跨线程投递超时（含同步渲染耗时） */
#define SAV_FLUSH_WAIT_MS 4000  /* 落屏等待（覆盖局刷 ~0.4s 与强制全刷 ~2s） */

#define SAV_MARGIN 12       /* 屏幕四周外边距 */
#define SAV_GAP 10          /* 盒间距 */
#define SAV_HEADER_H 32     /* 头部行高 */
#define SAV_FOOTER_H 22     /* 页脚行高 */
#define SAV_BORDER 2        /* 盒边框线宽 */
#define SAV_RADIUS 8        /* 盒圆角 */
#define SAV_BOX_PAD 6       /* 月历盒内边距 */
#define SAV_TITLE_H 26      /* 月历年月标题行高 */
#define SAV_WEEK_H 26       /* 月历星期表头行高 */

/* 月历网格规格。 */
#define SAV_CAL_COLS 7
#define SAV_CAL_ROWS 6

/* ------------------------------------------------------------------ */
/* 页面对象                                                             */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_header = NULL;    /*!< 头部：日期 + 农历 */
static lv_obj_t *s_wifi_icon = NULL; /*!< 头部右侧 WiFi 图标 */
static lv_obj_t *s_clock = NULL;     /*!< 时间盒：HH:MM 大字 */
static lv_obj_t *s_week = NULL;      /*!< 时间盒：星期 */
static lv_obj_t *s_w_icon = NULL;    /*!< 天气盒：天气图标 */
static lv_obj_t *s_w_text = NULL;    /*!< 天气盒：天况文字 */
static lv_obj_t *s_w_metrics = NULL; /*!< 天气盒：温/湿/风指标行 */
static bool s_weather_wide = false;  /*!< 天气盒是否为宽盒（决定指标是否含风） */
static lv_obj_t *s_cal_title = NULL; /*!< 月历盒：年月标题 */
static lv_obj_t *s_today_dot = NULL; /*!< 月历盒：今日反白圆标（日期标签下层） */
static lv_obj_t *s_cells[SAV_CAL_ROWS][SAV_CAL_COLS] = {0}; /*!< 月历日期格 */
static lv_obj_t *s_quote = NULL;     /*!< 一言盒：句子 */
static lv_obj_t *s_quote_from = NULL; /*!< 一言盒：出处作者行 */
static lv_obj_t *s_foot_ip = NULL;   /*!< 页脚：本机 IP */
static lv_obj_t *s_foot_ver = NULL;  /*!< 页脚：版本号 */
static lv_timer_t *s_timer = NULL;   /*!< 周期刷新定时器 */

/* 月历几何（enter 时填充，刷新时定位今日圆标；均为盒内相对坐标）。 */
static int s_cal_gx = 0;    /*!< 网格左上 x */
static int s_cal_gy = 0;    /*!< 网格首行顶 y */
static int s_cal_col_w = 0; /*!< 列宽 */
static int s_cal_row_h = 0; /*!< 行高 */

/* 内容 dedup 状态（变化才触碰标签 / 圆标，避免无谓 EPD 局刷）。 */
static int s_cal_ym = -1;    /*!< 已渲染年月（year*13+mon；-1=未渲染） */
static int s_today_row = -1; /*!< 已渲染今日行（-1=无） */
static int s_today_col = -1; /*!< 已渲染今日列 */

/* 天气摘要数据缓冲（快照较大，放 PSRAM，页面生命周期内复用）。 */
static espaperplay_weather_snapshot_t *s_snap = NULL;

static void sav_refresh(void); /*!< 内容刷新（enter 时先刷一次；定义见下方） */
static void sav_timer_cb(lv_timer_t *timer);

/** 时钟字号按屏幕短边分档（盒高不足时在 fill 内按盒高二次钳制）。 */
static int sav_clock_font_px(int32_t min_dim) {
    if (min_dim >= 400) {
        return 96;
    }
    if (min_dim >= 300) {
        return 68;
    }
    return 48;
}

/** 当前本地时间（NTP 已同步）或 NULL。 */
static const struct tm *sav_local_time(void) {
    static struct tm s_tm;
    if (espaperplay_clock_get_local_time(&s_tm) == ESP_OK &&
        s_tm.tm_year >= SAV_SYNCED_YEAR - 1900) {
        return &s_tm;
    }
    return NULL;
}

static const char *const s_weekday_zh[] = {"日", "一", "二", "三", "四", "五", "六"};

/** 某月 1 日是星期几（0=周日；公历，纯历法算术）。 */
static int sav_first_weekday(int year, int month) {
    int q = 1;
    int m = month, y = year;
    if (m < 3) {
        m += 12;
        y--;
    }
    const int k = y % 100, j = y / 100;
    return (q + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j + 6) % 7;
}

/** 某年某月天数（公历）。 */
static int sav_days_in_month(int year, int month) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return days[month - 1];
}

/** 按时段回退问候语（一言尚未取到时显示；参考稿「明天见」的用法）。 */
static const char *sav_greeting(const struct tm *tm) {
    if (tm == NULL) {
        return "你好";
    }
    const int h = tm->tm_hour;
    if (h >= 5 && h < 10) {
        return "早上好";
    }
    if (h >= 10 && h < 13) {
        return "中午好";
    }
    if (h >= 13 && h < 18) {
        return "下午好";
    }
    if (h >= 18 && h < 22) {
        return "晚上好";
    }
    return "明天见";
}

/* ------------------------------------------------------------------ */
/* 构建辅助                                                             */
/* ------------------------------------------------------------------ */

/** 圆角边框盒（白底黑框，禁滚动）。 */
static lv_obj_t *sav_box_create(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_width(box, SAV_BORDER, 0);
    lv_obj_set_style_radius(box, SAV_RADIUS, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

/* ------------------------------------------------------------------ */
/* 各盒构建                                                             */
/* ------------------------------------------------------------------ */

/** 时间盒：HH:MM 大字（按盒高钳制字号）+ 星期，整体居中。 */
static void sav_time_box_fill(lv_obj_t *box, int w, int h, int font_px) {
    const int week_h = 28;
    const int max_font = (h - week_h - 16) * 10 / 14;
    if (font_px > max_font) {
        font_px = max_font;
    }
    if (font_px < 32) {
        font_px = 32;
    }
    const int clock_h = font_px * 14 / 10;
    const int y0 = (h - clock_h - week_h) / 2;

    s_clock = espaperplay_ui_label_create(box, "--:--", font_px, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_clock, w);
    lv_obj_set_pos(s_clock, 0, y0);

    s_week = espaperplay_ui_label_create(box, "---", 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_week, w);
    lv_obj_set_pos(s_week, 0, y0 + clock_h);
}

/** 天气盒：单行仪表布局——图标 + 天况 + 指标行（温/湿[/风]，参考图
 * "一行放置"式；图标 64px 源绕中心缩放）。宽盒指标含风向风级，窄盒
 * （横屏左列）只放温/湿。 */
static void sav_weather_box_fill(lv_obj_t *box, int w, int h) {
    const int orn = (h - 16 < 56 ? h - 16 : 56); /* 图标显示尺寸（源 64x64） */
    const int text_f = orn >= 44 ? 20 : 16;
    const int met_f = text_f >= 20 ? 16 : 14;
    const int icon_end = 12 + orn + 8;
    s_weather_wide = w >= 420;

    s_w_icon = lv_image_create(box);
    lv_image_set_pivot(s_w_icon, 32, 32);
    lv_image_set_scale(s_w_icon, LV_SCALE_NONE * orn / 64);
    lv_obj_set_pos(s_w_icon, 12, h / 2 - 32);
    lv_obj_add_flag(s_w_icon, LV_OBJ_FLAG_HIDDEN);

    /* 天况：图标右侧起；宽盒为其预留到指标区之前的宽度（最长 6 字） */
    s_w_text = espaperplay_ui_label_create(box, "", text_f, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_w_text, icon_end, (h - text_f * 14 / 10) / 2);
    lv_obj_set_width(s_w_text, s_weather_wide ? (w * 2 / 5 - icon_end) : (w - icon_end - 12));

    /* 指标行：右对齐贴盒右缘。宽盒从盒宽一半起（天况最长 6 字也不会
     * 越界）；窄盒（横屏左列 336px）只放温/湿。 */
    const int met_x = s_weather_wide ? (w / 2) : (w * 12 / 25);
    s_w_metrics = espaperplay_ui_label_create(box, "", met_f, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_pos(s_w_metrics, met_x, (h - met_f * 14 / 10) / 2);
    lv_obj_set_width(s_w_metrics, w - met_x - 10);
}

/** 月历盒：年月标题 + 星期表头（周末反色小块）+ 6x7 日期网格。
 * 圆标先建（落在日期标签下层）；网格几何存静态供刷新定位（盒内相对坐标）。 */
static void sav_cal_box_fill(lv_obj_t *box, int w, int h) {
    const int gx = SAV_BOX_PAD;
    const int gw = w - 2 * SAV_BOX_PAD;
    const int col_w = gw / SAV_CAL_COLS;
    const int row_h = (h - 2 * SAV_BOX_PAD - SAV_TITLE_H - SAV_WEEK_H - 2) / SAV_CAL_ROWS;
    const int cell_f = col_w < 44 ? 16 : 20;
    const int cell_h = cell_f * 14 / 10;
    const int wk_y = SAV_BOX_PAD + SAV_TITLE_H;
    const int gy = wk_y + SAV_WEEK_H;

    s_cal_gx = gx;
    s_cal_gy = gy;
    s_cal_col_w = col_w;
    s_cal_row_h = row_h;

    /* 年月标题 */
    s_cal_title = espaperplay_ui_label_create(box, "", 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_cal_title, gw);
    lv_obj_set_pos(s_cal_title, gx, SAV_BOX_PAD);

    /* 星期表头：周末（日/六）反色小块 + 白字，其余黑字 */
    for (int c = 0; c < SAV_CAL_COLS; c++) {
        const int cx = gx + c * col_w + col_w / 2;
        const bool weekend = (c == 0 || c == 6);
        if (weekend) {
            lv_obj_t *chip = lv_obj_create(box);
            lv_obj_set_size(chip, 28, 20);
            lv_obj_set_pos(chip, cx - 14, wk_y + 3);
            lv_obj_set_style_bg_color(chip, lv_color_black(), 0);
            lv_obj_set_style_border_width(chip, 0, 0);
            lv_obj_set_style_radius(chip, 4, 0);
            lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "%s", s_weekday_zh[c]);
        lv_obj_t *l = espaperplay_ui_label_create(box, buf, cell_f, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(l, col_w);
        lv_obj_set_pos(l, gx + c * col_w, wk_y + (SAV_WEEK_H - cell_h) / 2);
        if (weekend) {
            lv_obj_set_style_text_color(l, lv_color_white(), 0);
        }
    }

    /* 今日圆标（先建 → 落在日期标签下层） */
    const int dot_d = (row_h < col_w ? row_h : col_w) - 6;
    s_today_dot = lv_obj_create(box);
    lv_obj_set_size(s_today_dot, dot_d, dot_d);
    lv_obj_set_style_bg_color(s_today_dot, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_today_dot, 0, 0);
    lv_obj_set_style_radius(s_today_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(s_today_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_today_dot, LV_OBJ_FLAG_HIDDEN);

    /* 日期网格（空文本占位，刷新时 dedup 填充） */
    for (int r = 0; r < SAV_CAL_ROWS; r++) {
        for (int c = 0; c < SAV_CAL_COLS; c++) {
            lv_obj_t *l = espaperplay_ui_label_create(box, "", cell_f, LV_TEXT_ALIGN_CENTER);
            lv_obj_set_width(l, col_w);
            lv_obj_set_pos(l, gx + c * col_w, gy + r * row_h + (row_h - cell_h) / 2);
            s_cells[r][c] = l;
        }
    }
}

/** 一言盒：句子（自动换行居中）+ 出处作者行（右对齐，可空）。 */
static void sav_quote_box_fill(lv_obj_t *box, int w, int h) {
    const int from_h = 22;
    const int quote_f = h >= 96 ? 18 : 16;
    const int pad = 10;

    s_quote = espaperplay_ui_label_create(box, "", quote_f, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_quote, w - 2 * pad);
    lv_obj_set_pos(s_quote, pad, pad);

    s_quote_from = espaperplay_ui_label_create(box, "", 16, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_width(s_quote_from, w - 2 * pad);
    lv_obj_set_pos(s_quote_from, pad, h - from_h - 6);
}

/* ------------------------------------------------------------------ */
/* 屏保构建（页面 enter）                                                */
/* ------------------------------------------------------------------ */

/** 竖屏：单列自上而下 时间/天气/月历/一言；横屏：左列 时间/天气/一言，
 * 右列月历（参考构图）。盒高按剩余空间比例分配。 */
static void sav_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    s_cal_ym = -1;
    s_today_row = -1;
    s_today_col = -1;

    int32_t scr_w, scr_h;
    espaperplay_ui_screen_size(&scr_w, &scr_h);
    const int32_t min_dim = scr_w < scr_h ? scr_w : scr_h;
    const int W = (int)scr_w, H = (int)scr_h;
    const bool portrait = scr_w < scr_h;
    const int header_f = min_dim >= 400 ? 24 : 20;

    /* 头部行：日期+农历 左，WiFi 图标 右 */
    s_header = espaperplay_ui_label_create(scr, "", header_f, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(s_header, W - 2 * SAV_MARGIN - 28);
    lv_obj_set_pos(s_header, SAV_MARGIN, SAV_MARGIN + (SAV_HEADER_H - header_f * 14 / 10) / 2);
    s_wifi_icon = lv_image_create(scr);
    lv_obj_set_pos(s_wifi_icon, W - SAV_MARGIN - 16, SAV_MARGIN + SAV_HEADER_H / 2 - 8);

    /* 左列盒宽：竖屏 = 全宽；横屏 = 42% 列宽 */
    const int left_w = portrait ? (W - 2 * SAV_MARGIN) : (W * 42 / 100);
    const int boxes_h = H - 2 * SAV_MARGIN - SAV_HEADER_H - SAV_FOOTER_H - 4 * SAV_GAP;
    int time_h, weather_h, cal_h, quote_h;
    if (portrait) {
        time_h = boxes_h * 27 / 100;
        weather_h = boxes_h * 12 / 100; /* 单行仪表布局：图标+天况+指标一行 */
        cal_h = boxes_h * 45 / 100;
        quote_h = boxes_h - time_h - weather_h - cal_h;
    } else {
        const int mid_h = H - 2 * SAV_MARGIN - SAV_HEADER_H - SAV_FOOTER_H - 2 * SAV_GAP;
        time_h = mid_h * 38 / 100;
        weather_h = mid_h * 18 / 100;
        quote_h = mid_h - time_h - weather_h - 2 * SAV_GAP;
        cal_h = mid_h;
    }

    /* 月历盒位置：竖屏在天气盒之下；横屏占右侧整列 */
    const int cal_x = portrait ? SAV_MARGIN : (SAV_MARGIN + left_w + SAV_GAP);
    const int cal_y =
        portrait ? (SAV_MARGIN + SAV_HEADER_H + SAV_GAP + time_h + SAV_GAP + weather_h + SAV_GAP)
                 : (SAV_MARGIN + SAV_HEADER_H + SAV_GAP);
    const int cal_w = portrait ? (W - 2 * SAV_MARGIN)
                               : (W - 2 * SAV_MARGIN - left_w - SAV_GAP);

    /* 时间盒 / 天气盒 / 一言盒（左列，自上而下） */
    int y = SAV_MARGIN + SAV_HEADER_H + SAV_GAP;
    sav_time_box_fill(sav_box_create(scr, SAV_MARGIN, y, left_w, time_h), left_w, time_h,
                      sav_clock_font_px(min_dim));
    y += time_h + SAV_GAP;
    sav_weather_box_fill(sav_box_create(scr, SAV_MARGIN, y, left_w, weather_h), left_w,
                         weather_h);
    y += weather_h + SAV_GAP;

    /* 月历盒（竖屏：左列第三格；横屏：右列） */
    sav_cal_box_fill(sav_box_create(scr, cal_x, cal_y, cal_w, cal_h), cal_w, cal_h);

    /* 一言盒（竖屏：左列第四格；横屏：左列第三格） */
    if (portrait) {
        y = cal_y + cal_h + SAV_GAP;
    }
    sav_quote_box_fill(sav_box_create(scr, SAV_MARGIN, y, left_w, quote_h), left_w, quote_h);

    /* 页脚：本机 IP 左、版本右 */
    const int foot_y = H - SAV_MARGIN - SAV_FOOTER_H + 3;
    s_foot_ip = espaperplay_ui_label_create(scr, "", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(s_foot_ip, W / 2);
    lv_obj_set_pos(s_foot_ip, SAV_MARGIN, foot_y);
    s_foot_ver = espaperplay_ui_label_create(scr, "", 16, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_width(s_foot_ver, W / 2 - SAV_MARGIN);
    lv_obj_set_pos(s_foot_ver, W / 2, foot_y);

    s_timer = lv_timer_create(sav_timer_cb, SAV_UI_PERIOD_MS, NULL);
    if (s_timer == NULL) {
        ESP_LOGE(TAG, "screensaver: periodic refresh timer create failed");
    }
    sav_refresh();

    /* 一言尚无内容（首次开机/持续失败）：催一次立即拉取，避免展示长期
     * 停留在问候语回退。 */
    {
        espaperplay_hitokoto_t h;
        espaperplay_hitokoto_get(&h);
        if (!h.valid) {
            espaperplay_hitokoto_request_refresh();
        }
    }

    /* 睡眠期间分钟对齐周期唤醒：屏保时钟随分钟更新（与主界面同一
     * 机制；主界面 exit 时已关闭，此处接手）。弹出后由 home.enter 重开。 */
    espaperplay_power_set_periodic_wakeup_minute_aligned(true);

    ESP_LOGI(TAG, "screensaver entered");
}

/* ------------------------------------------------------------------ */
/* 内容刷新                                                             */
/* ------------------------------------------------------------------ */

/** 头部：公历日期 + 农历（dedup）。 */
static void sav_refresh_header(const struct tm *tm) {
    char buf[80];
    if (tm == NULL) {
        snprintf(buf, sizeof(buf), "正在同步时间…");
    } else {
        char lunar[16];
        if (espaperplay_clock_lunar_text(tm, lunar, sizeof(lunar)) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d 农历:%s", tm->tm_year + 1900,
                     tm->tm_mon + 1, tm->tm_mday, lunar);
        } else {
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1,
                     tm->tm_mday);
        }
    }
    espaperplay_ui_label_set_text_dedup(s_header, buf);
}

/** 头部 WiFi 图标（源变化才换）。 */
static void sav_refresh_wifi_icon(void) {
    const lv_image_dsc_t *icon;
    int rssi = 0;
    if (espaperplay_wifi_is_sta_online() && espaperplay_wifi_get_rssi(&rssi) == ESP_OK) {
        icon = espaperplay_ui_wifi_rssi_icon(rssi);
    } else {
        icon = &icon_wifi_off_16;
    }
    if (lv_image_get_src(s_wifi_icon) != (const void *)icon) {
        lv_image_set_src(s_wifi_icon, icon);
    }
}

/** 时间盒：HH:MM + 星期（dedup）。 */
static void sav_refresh_clock(const struct tm *tm) {
    char buf[16];
    if (tm != NULL) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
        espaperplay_ui_label_set_text_dedup(s_clock, buf);
        snprintf(buf, sizeof(buf), "星期%s", s_weekday_zh[tm->tm_wday]);
        espaperplay_ui_label_set_text_dedup(s_week, buf);
    } else {
        espaperplay_ui_label_set_text_dedup(s_clock, "--:--");
        espaperplay_ui_label_set_text_dedup(s_week, "星期-");
    }
}

/** 天气盒：图标（实时天气回退月相）+ 天况 + 温/湿[/风]指标（dedup）。 */
static void sav_refresh_weather(void) {
    char text[48], met[96];
    if (s_snap == NULL) {
        s_snap = heap_caps_malloc(sizeof(*s_snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    const lv_image_dsc_t *ic = NULL;
    if (s_snap != NULL && espaperplay_weather_get_snapshot(s_snap) == ESP_OK && s_snap->valid) {
        snprintf(text, sizeof(text), "%s", s_snap->now.text);
        if (s_weather_wide && s_snap->now.wind_dir[0] != '\0') {
            snprintf(met, sizeof(met), "T:%s℃  H:%s%%  %s%s%s", s_snap->now.temp,
                     s_snap->now.humidity, s_snap->now.wind_dir, s_snap->now.wind_scale,
                     s_snap->now.wind_scale[0] != '\0' ? "级" : "");
        } else {
            snprintf(met, sizeof(met), "T:%s℃  H:%s%%", s_snap->now.temp,
                     s_snap->now.humidity);
        }
        ic = qweather_icon_get(s_snap->now.icon);
        if (ic == NULL && s_snap->astronomy.moon_phase_icon[0] != '\0') {
            ic = qweather_icon_get(s_snap->astronomy.moon_phase_icon);
        }
    } else {
        snprintf(text, sizeof(text), "天气未配置");
        met[0] = '\0';
    }
    espaperplay_ui_label_set_text_dedup(s_w_text, text);
    espaperplay_ui_label_set_text_dedup(s_w_metrics, met);

    if (ic != NULL) {
        if (lv_image_get_src(s_w_icon) != (const void *)ic) {
            lv_image_set_src(s_w_icon, ic);
        }
        lv_obj_remove_flag(s_w_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_w_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 月历盒：年月标题 + 日期网格 + 今日圆标（仅月份或今日变化时触碰网格）。 */
static void sav_refresh_calendar(const struct tm *tm) {
    if (tm == NULL) {
        espaperplay_ui_label_set_text_dedup(s_cal_title, "----年--月");
        return;
    }
    const int year = tm->tm_year + 1900;
    const int mon = tm->tm_mon + 1; /* 1..12 */
    const int ym = year * 13 + mon;
    const int first_wd = sav_first_weekday(year, mon);

    if (ym != s_cal_ym) {
        /* 月份切换：整面重铺（标签 dedup，未变的格子不触发局刷）。 */
        s_cal_ym = ym;
        char title[32];
        snprintf(title, sizeof(title), "%d年%d月", year, mon);
        espaperplay_ui_label_set_text_dedup(s_cal_title, title);

        const int days = sav_days_in_month(year, mon);
        for (int r = 0; r < SAV_CAL_ROWS; r++) {
            for (int c = 0; c < SAV_CAL_COLS; c++) {
                const int idx = r * SAV_CAL_COLS + c - first_wd;
                char buf[8] = "";
                if (idx >= 0 && idx < days) {
                    snprintf(buf, sizeof(buf), "%d", idx + 1);
                }
                espaperplay_ui_label_set_text_dedup(s_cells[r][c], buf);
                /* 上月今日格的白字复位黑字（今日格下方单独处理）。 */
                lv_obj_set_style_text_color(s_cells[r][c], lv_color_black(), 0);
            }
        }
        s_today_row = -1;
        s_today_col = -1;
    }

    /* 今日圆标（跨格移动只触碰两枚标签的着色与圆标坐标） */
    const int idx = first_wd + tm->tm_mday - 1;
    const int r = idx / SAV_CAL_COLS, c = idx % SAV_CAL_COLS;
    if (r != s_today_row || c != s_today_col) {
        if (s_today_row >= 0) {
            lv_obj_set_style_text_color(s_cells[s_today_row][s_today_col], lv_color_black(), 0);
        }
        s_today_row = r;
        s_today_col = c;
        lv_obj_t *cell = s_cells[r][c];
        lv_obj_set_style_text_color(cell, lv_color_white(), 0);
        const int dot_d = (s_cal_row_h < s_cal_col_w ? s_cal_row_h : s_cal_col_w) - 6;
        lv_obj_set_size(s_today_dot, dot_d, dot_d);
        lv_obj_set_pos(s_today_dot, s_cal_gx + c * s_cal_col_w + s_cal_col_w / 2 - dot_d / 2,
                       s_cal_gy + r * s_cal_row_h + s_cal_row_h / 2 - dot_d / 2);
        lv_obj_remove_flag(s_today_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 一言盒：Hitokoto 句子 + 出处作者；未取到时按时段回退问候（dedup）。 */
static void sav_refresh_quote(const struct tm *tm) {
    espaperplay_hitokoto_t h;
    espaperplay_hitokoto_get(&h);

    char from[192];
    if (h.valid) {
        espaperplay_ui_label_set_text_dedup(s_quote, h.text);
        if (h.from[0] != '\0' && h.from_who[0] != '\0') {
            snprintf(from, sizeof(from), "——《%s》· %s", h.from, h.from_who);
        } else if (h.from[0] != '\0') {
            snprintf(from, sizeof(from), "——《%s》", h.from);
        } else if (h.from_who[0] != '\0') {
            snprintf(from, sizeof(from), "——%s", h.from_who);
        } else {
            from[0] = '\0';
        }
    } else {
        espaperplay_ui_label_set_text_dedup(s_quote, sav_greeting(tm));
        from[0] = '\0';
    }
    espaperplay_ui_label_set_text_dedup(s_quote_from, from);
}

/** 页脚：本机 IP + 版本（dedup）。
 * IP 取最近一次有效值：入睡流程先渲屏保后挂起 WiFi（modem 断电主动
 * 断开会把状态清成 0.0.0.0），睡眠期间分钟唤醒刷新会把清零值刷上屏；
 * 海报上应持续显示可回访的管理页地址，故缓存到本次睡眠前为止的 IP。 */
static void sav_refresh_footer(void) {
    static char s_last_ip[16] = ""; /* 最近一次有效 IPv4（跨唤醒保持） */
    char buf[48];
    espaperplay_wifi_status_t st;
    if (espaperplay_wifi_get_status(&st) == ESP_OK && st.ip[0] != '\0' &&
        strcmp(st.ip, "0.0.0.0") != 0) {
        strlcpy(s_last_ip, st.ip, sizeof(s_last_ip));
    }
    snprintf(buf, sizeof(buf), "IP:%s", s_last_ip[0] != '\0' ? s_last_ip : "--");
    espaperplay_ui_label_set_text_dedup(s_foot_ip, buf);
    snprintf(buf, sizeof(buf), "v%s", ESPAPERPLAY_VERSION);
    espaperplay_ui_label_set_text_dedup(s_foot_ver, buf);
}

/** 刷新全部内容（头部 / WiFi / 时钟 / 天气 / 月历 / 一言 / 页脚；
 * 全部 dedup：内容未变化不触发 EPD 局刷）。 */
static void sav_refresh(void) {
    const struct tm *tm = sav_local_time();

    sav_refresh_header(tm);
    sav_refresh_wifi_icon();
    sav_refresh_clock(tm);
    sav_refresh_weather();
    sav_refresh_calendar(tm);
    sav_refresh_quote(tm);
    sav_refresh_footer();
}

/** 周期刷新（LVGL 线程内，lv_timer 驱动）。 */
static void sav_timer_cb(lv_timer_t *timer) {
    (void)timer;
    sav_refresh();
}

/** 屏保退出（页面 exit）。 */
static void sav_exit(void) {
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    espaperplay_power_set_periodic_wakeup_minute_aligned(false);
    ESP_LOGI(TAG, "screensaver exited");
}

/* ------------------------------------------------------------------ */
/* 自退出 / 页面实例                                                     */
/* ------------------------------------------------------------------ */

/** 屏保是否为当前栈顶页（LVGL 线程内；栈内是页面实例的按值副本，
 * 与全局实例地址永不相等，须比较函数指针）。 */
static bool sav_is_top(void) {
    const espaperplay_ui_page_t *top = espaperplay_ui_page_top_lv();
    return top != NULL && top->enter == espaperplay_ui_page_screensaver.enter;
}

/** 屏保自退出（LVGL 线程内，on_touch / on_key / 电源 dismiss 投递调用）。
 * 退出即回到交互态：同步清除睡眠指示，防止「睡前守卫拦截后用户操作、
 * 设备保持唤醒」场景下主界面状态栏滞留节能图标（与电源清除路径幂等）。 */
static void sav_dismiss_self(void) {
    if (sav_is_top()) {
        espaperplay_input_set_sleep_indicator(false);
        espaperplay_ui_page_pop_lv();
    }
}

/** 屏保触摸处理：任意按压即退出屏保、回到主界面。 */
static void sav_on_touch(const espaperplay_input_event_t *event) {
    if (event->touch_pressed) {
        sav_dismiss_self();
    }
}

/** 屏保按键处理：任意按键按下即退出屏保。 */
static void sav_on_key(const espaperplay_input_event_t *event) {
    if (event->type == ESPAPERPLAY_INPUT_EVENT_KEY &&
        event->key_action == ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_DOWN) {
        sav_dismiss_self();
    }
}

/** 屏保页面实例（页面栈用；由电源管理经 show/dismiss API 驱动进出）。 */
const espaperplay_ui_page_t espaperplay_ui_page_screensaver = {sav_enter, sav_exit, sav_on_key,
                                                               sav_on_touch};

/* ------------------------------------------------------------------ */
/* 跨线程驱动（电源管理任务调用）                                        */
/* ------------------------------------------------------------------ */

/** gui_lv_call 包装：压入屏保并同步渲染（LVGL 线程内）。 */
static void sav_show_cb(void *arg) {
    bool *shown = arg;
    *shown = false;

    /* 重试路径：上次入睡被睡前守卫拦截时屏保已在栈顶，无需重推。 */
    if (sav_is_top()) {
        *shown = true;
        return;
    }
    /* 触发条件：栈顶为主界面（系统在主页进入睡眠才显示屏保）。 */
    const espaperplay_ui_page_t *top = espaperplay_ui_page_top_lv();
    if (top == NULL || top->enter != espaperplay_ui_page_home.enter) {
        return;
    }
    if (espaperplay_ui_page_push_lv(&espaperplay_ui_page_screensaver) == ESP_OK) {
        *shown = true;
        /* 同步渲染首帧并入队 EPD 刷新：投递回调返回后调用方才能用
         * wait_idle 等到「已排队」的刷新——否则渲染尚未发生，wait_idle
         * 立即空转返回，随后冻结管线会丢帧，睡眠期间屏幕停留旧画面。 */
        lv_refr_now(NULL);
    }
}

bool espaperplay_ui_screensaver_show(void) {
    bool shown = false;
    if (espaperplay_gui_lv_call(sav_show_cb, &shown, SAV_LV_CALL_MS) != ESP_OK) {
        return false;
    }
    if (!shown) {
        return false;
    }
    /* 等待首帧真正落到面板（EPD 刷新异步执行于 worker）。 */
    espaperplay_gui_wait_idle(SAV_FLUSH_WAIT_MS);
    return true;
}

/** gui_lv_call 包装：弹出屏保、重建主界面（LVGL 线程内）。 */
static void sav_dismiss_cb(void *arg) {
    (void)arg;
    sav_dismiss_self();
}

void espaperplay_ui_screensaver_dismiss(void) {
    (void)espaperplay_gui_lv_call(sav_dismiss_cb, NULL, SAV_LV_CALL_MS);
}
