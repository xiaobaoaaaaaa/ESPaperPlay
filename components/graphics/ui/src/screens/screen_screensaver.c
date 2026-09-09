/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "espaperplay_clock.h"
#include "espaperplay_gui.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_input.h"
#include "espaperplay_power.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_util.h"
#include "espaperplay_weather.h"

#include "qweather_icons.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 睡眠屏保页
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
 * 版式（海报式居中构图，横竖屏同一套公式；MCP 半比例渲染在
 * 480x800 / 800x480 / 480x640 / 320x480 四档验证通过）：
 *   - 全屏细边框海报框（内缩 14px，radius 16，2px）；
 *   - 日期眉行（24px，字距 +2）；
 *   - 翻页时钟双卡：时/分各一圆角卡（宽 = 字号*1.7，高 = 字号*1.3，
 *     3px 边框），分钟卡反色（黑底白字）作版面视觉锚点；卡间中缝上下
 *     两枚实心圆点作冒号（静态，EPD 无动画）；
 *   - 装饰行：短横线 + 实时天气图标（未收录回退月相图标，再回退单根
 *     短线）+ 短横线；
 *   - 天气摘要行（20px）；页脚（16px）：版本 · 触摸唤醒。
 * 时钟字号按屏幕短边分档（>=400→96 / >=300→68 / 其余→48），卡体、圆点、
 * 间距全部由字号派生，文本行字号随短边降档；整个内容栈在框内垂直居中，
 * 任何分辨率不溢出、不重叠。文本 dedup 刷新（分钟跳变只局刷两张卡）。
 */

#define SAV_UI_PERIOD_MS 1000   /* 内容轮询周期（dedup：内容未变不刷新 EPD） */
#define SAV_SYNCED_YEAR 2024    /* 判定 NTP 已同步的最小年份 */
#define SAV_LV_CALL_MS 4000     /* 跨线程投递超时（含同步渲染耗时） */
#define SAV_FLUSH_WAIT_MS 4000  /* 落屏等待（覆盖局刷 ~0.4s 与强制全刷 ~2s） */

#define SAV_FRAME_INSET 14    /* 海报框内缩（px） */
#define SAV_FRAME_RADIUS 16   /* 海报框圆角 */
#define SAV_FRAME_BORDER 2    /* 海报框线宽 */
#define SAV_CARD_RADIUS 14    /* 时钟卡圆角 */
#define SAV_CARD_BORDER 3     /* 时钟卡线宽 */
#define SAV_DATE_FONT 24      /* 日期眉行字号（短边 <400 降为 20） */
#define SAV_WTHR_FONT 20      /* 天气行字号（短边 <400 降为 16） */
#define SAV_FOOT_FONT 16      /* 页脚字号 */

static lv_obj_t *s_hour_label = NULL;    /*!< 时数字（时卡内居中） */
static lv_obj_t *s_min_label = NULL;     /*!< 分数字（反色分卡内居中） */
static lv_obj_t *s_date = NULL;          /*!< 日期眉行 */
static lv_obj_t *s_icon = NULL;          /*!< 装饰行图标（天气/月相，64px 源缩放） */
static lv_obj_t *s_line_l = NULL;        /*!< 装饰行左短线 */
static lv_obj_t *s_line_r = NULL;        /*!< 装饰行右短线 */
static lv_obj_t *s_rule = NULL;          /*!< 无图标时的单根短线 */
static lv_obj_t *s_weather = NULL;       /*!< 天气摘要行 */
static lv_obj_t *s_footer = NULL;        /*!< 版本 / 唤醒提示 */
static lv_timer_t *s_timer = NULL;       /*!< 周期刷新定时器 */

/* 天气摘要数据缓冲（快照较大，放 PSRAM，页面生命周期内复用）。 */
static espaperplay_weather_snapshot_t *s_snap = NULL;

static const char *const s_weekday_zh[] = {"日", "一", "二", "三", "四", "五", "六"};

static void sav_refresh(void); /*!< 内容刷新（enter 时先刷一次；定义见下方） */
static void sav_timer_cb(lv_timer_t *timer);

/** 时钟字号按屏幕短边分档：卡体由字号派生，保证窄屏不溢出。 */
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

/* ------------------------------------------------------------------ */
/* 版式构建                                                             */
/* ------------------------------------------------------------------ */

/** 时钟卡：圆角边框卡 + 居中数字；inv=true 反色（黑底白字）。 */
static void sav_card_create(lv_obj_t *parent, int x, int y, int w, int h, int font_px, bool inv) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, inv ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, SAV_CARD_BORDER, 0);
    lv_obj_set_style_radius(card, SAV_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "--");
    lv_font_t *font = espaperplay_ui_font(font_px);
    if (font != NULL) {
        lv_obj_set_style_text_font(t, font, 0);
    }
    lv_obj_set_style_text_color(t, inv ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(t);

    if (inv) {
        s_min_label = t;
    } else {
        s_hour_label = t;
    }
}

/** 细横线（装饰用）。 */
static lv_obj_t *sav_rule_create(lv_obj_t *parent, int x, int y, int w) {
    lv_obj_t *ln = lv_obj_create(parent);
    lv_obj_set_size(ln, w, 2);
    lv_obj_set_pos(ln, x, y);
    lv_obj_set_style_bg_color(ln, lv_color_black(), 0);
    lv_obj_set_style_border_width(ln, 0, 0);
    lv_obj_set_style_radius(ln, 0, 0);
    return ln;
}

/** 屏保构建（页面 enter：屏幕已由页面栈清空）。 */
static void sav_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    int32_t scr_w, scr_h;
    espaperplay_ui_screen_size(&scr_w, &scr_h);
    const int32_t min_dim = scr_w < scr_h ? scr_w : scr_h;
    const int f = sav_clock_font_px(min_dim);
    const int date_f = min_dim >= 400 ? SAV_DATE_FONT : 20;
    const int wthr_f = min_dim >= 400 ? SAV_WTHR_FONT : 16;

    /* 海报框（全部子元素挂框内，坐标相对框原点） */
    lv_obj_t *frame = lv_obj_create(scr);
    lv_obj_set_size(frame, scr_w - 2 * SAV_FRAME_INSET, scr_h - 2 * SAV_FRAME_INSET);
    lv_obj_set_pos(frame, SAV_FRAME_INSET, SAV_FRAME_INSET);
    lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_set_style_border_width(frame, SAV_FRAME_BORDER, 0);
    lv_obj_set_style_radius(frame, SAV_FRAME_RADIUS, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    /* 垂直流式栈（间距全部由时钟字号派生），整体在框内垂直居中 */
    const int box_w = f * 17 / 10, box_h = f * 13 / 10, colgap = f * 30 / 100;
    const int g0 = f * 30 / 100; /* 眉行 -> 时钟 */
    const int g1 = f * 30 / 100; /* 时钟 -> 装饰行 */
    const int g2 = f * 14 / 100; /* 装饰行 -> 天气 */
    const int g3 = f * 55 / 100; /* 天气 -> 页脚 */
    const int orn = f * 42 / 100; /* 装饰行图标区高度 */
    const int date_h = date_f * 14 / 10, wthr_h = wthr_f * 14 / 10,
              foot_h = SAV_FOOT_FONT * 14 / 10;
    const int stack = date_h + g0 + box_h + g1 + orn + g2 + wthr_h + g3 + foot_h;
    const int W = (int)scr_w - 2 * SAV_FRAME_INSET;
    int y = ((int)scr_h - 2 * SAV_FRAME_INSET - stack) / 2;
    const int text_w = W - 16; /* 文本行两侧各留 8px */

    /* 日期眉行 */
    s_date = espaperplay_ui_label_create(frame, "", date_f, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(s_date, 2, 0);
    lv_obj_set_width(s_date, text_w);
    lv_obj_set_pos(s_date, 8, y);
    y += date_h + g0;

    /* 翻页时钟双卡（分钟卡反色）+ 中缝冒号圆点 */
    const int group_w = 2 * box_w + colgap;
    const int bx = (W - group_w) / 2;
    sav_card_create(frame, bx, y, box_w, box_h, f, false);
    sav_card_create(frame, bx + box_w + colgap, y, box_w, box_h, f, true);
    const int dot = f * 14 / 100;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *d = lv_obj_create(frame);
        lv_obj_set_size(d, dot, dot);
        lv_obj_set_pos(d, W / 2 - dot / 2, y + box_h * (i == 0 ? 30 : 70) / 100 - dot / 2);
        lv_obj_set_style_bg_color(d, lv_color_black(), 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    }
    y += box_h + g1;

    /* 装饰行：短线 + 天气/月相图标 + 短线；无图标回退单根短线。
     * 图标源 64x64，缩放至 orn 并绕中心变换：布局尺寸仍为源尺寸，
     * 以视觉中心定位（pos = 中心 - 源一半，pivot 设为源中心）。 */
    const int line_w = f * 80 / 100, lgap = 10;
    s_line_l = sav_rule_create(frame, W / 2 - orn / 2 - lgap - line_w, y + orn / 2 - 1, line_w);
    s_line_r = sav_rule_create(frame, W / 2 + orn / 2 + lgap, y + orn / 2 - 1, line_w);

    s_icon = lv_image_create(frame);
    lv_image_set_pivot(s_icon, 32, 32);
    lv_image_set_scale(s_icon, LV_SCALE_NONE * orn / 64);
    lv_obj_set_pos(s_icon, W / 2 - 32, y + orn / 2 - 32);
    lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);

    s_rule = sav_rule_create(frame, W / 2 - f * 80 / 100, y + orn / 2 - 1, f * 160 / 100);
    lv_obj_add_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
    y += orn + g2;

    /* 天气摘要行 */
    s_weather = espaperplay_ui_label_create(frame, "", wthr_f, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_weather, text_w);
    lv_obj_set_pos(s_weather, 8, y);
    y += wthr_h + g3;

    /* 页脚 */
    s_footer = espaperplay_ui_label_create(frame, "", SAV_FOOT_FONT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(s_footer, 2, 0);
    lv_obj_set_width(s_footer, text_w);
    lv_obj_set_pos(s_footer, 8, y);

    s_timer = lv_timer_create(sav_timer_cb, SAV_UI_PERIOD_MS, NULL);
    if (s_timer == NULL) {
        ESP_LOGE(TAG, "screensaver: periodic refresh timer create failed");
    }
    sav_refresh();

    /* 睡眠期间分钟对齐周期唤醒：屏保时钟随分钟更新（与主界面同一
     * 机制；主界面 exit 时已关闭，此处接手）。弹出后由 home.enter 重开。 */
    espaperplay_power_set_periodic_wakeup_minute_aligned(true);

    ESP_LOGI(TAG, "screensaver entered");
}

/* ------------------------------------------------------------------ */
/* 内容刷新                                                             */
/* ------------------------------------------------------------------ */

/** 刷新时钟卡 / 日期 / 装饰图标 / 天气 / 页脚（内容变化才重绘）。 */
static void sav_refresh(void) {
    char buf[192];
    const struct tm *tm = sav_local_time();

    /* 时钟双卡 + 日期眉行 */
    if (tm != NULL) {
        snprintf(buf, sizeof(buf), "%02d", tm->tm_hour);
        espaperplay_ui_label_set_text_dedup(s_hour_label, buf);
        snprintf(buf, sizeof(buf), "%02d", tm->tm_min);
        espaperplay_ui_label_set_text_dedup(s_min_label, buf);
        snprintf(buf, sizeof(buf), "%04d年%02d月%02d日 星期%s", tm->tm_year + 1900,
                 tm->tm_mon + 1, tm->tm_mday, s_weekday_zh[tm->tm_wday]);
    } else {
        espaperplay_ui_label_set_text_dedup(s_hour_label, "--");
        espaperplay_ui_label_set_text_dedup(s_min_label, "--");
        snprintf(buf, sizeof(buf), "正在同步时间…");
    }
    espaperplay_ui_label_set_text_dedup(s_date, buf);

    /* 天气摘要 + 装饰行图标（实时天气图标优先，未收录回退月相图标） */
    if (s_snap == NULL) {
        s_snap = heap_caps_malloc(sizeof(*s_snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    const lv_image_dsc_t *ic = NULL;
    if (s_snap != NULL && espaperplay_weather_get_snapshot(s_snap) == ESP_OK && s_snap->valid) {
        snprintf(buf, sizeof(buf), "%s · %s %s℃  湿度 %s%%", s_snap->location_name,
                 s_snap->now.text, s_snap->now.temp, s_snap->now.humidity);
        ic = qweather_icon_get(s_snap->now.icon);
        if (ic == NULL && s_snap->astronomy.moon_phase_icon[0] != '\0') {
            ic = qweather_icon_get(s_snap->astronomy.moon_phase_icon);
        }
    } else {
        snprintf(buf, sizeof(buf), "天气：未配置或不可用");
    }
    espaperplay_ui_label_set_text_dedup(s_weather, buf);

    if (ic != NULL) {
        if (lv_image_get_src(s_icon) != (const void *)ic) {
            lv_image_set_src(s_icon, ic);
        }
        lv_obj_remove_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_line_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_line_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_line_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_line_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
    }

    /* 页脚 */
    snprintf(buf, sizeof(buf), "v%s · 触摸唤醒", ESPAPERPLAY_VERSION);
    espaperplay_ui_label_set_text_dedup(s_footer, buf);
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
