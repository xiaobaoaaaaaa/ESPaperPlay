/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "espaperplay_ui_util.h"
#include "espaperplay_ui_gesture.h"
#include "espaperplay_ui_modal.h"
#include "espaperplay_input.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"
#include "espaperplay_weather.h"
#include "icons_data.h"
#include "qweather_icons.h"

/* Android 式应用标题 + 明确的标签导航。黑白墨水屏用间距、字级、
 * 轮廓和下划线表达层次，切换无动画。预报不再依赖可滚动的双曲线。
 * 小时预报每屏 12 小时、每 3 小时一列；点击或滑动该区域查看后半天。
 * 太阳/月亮只作为天文项目图标，真实月相明确用文字显示。 */
#define WEATHER_TABS 3
#define WEATHER_CONTENT_Y 194
#define WEATHER_PERIOD_MS 30000

static espaperplay_ui_status_bar_t *s_bar;
static lv_obj_t *s_content, *s_location, *s_updated, *s_tabs[WEATHER_TABS];
static lv_obj_t *s_detail;
static lv_timer_t *s_timer;
static espaperplay_weather_snapshot_t *s_snapshot;
static int s_page, s_hour_group, s_index_group, s_warning_index;
static bool s_touch_down, s_valid;
static lv_point_t s_start, s_last;
static char s_update[32], s_hint[80];
static unsigned s_retry_count;
static int s_width, s_height;

static const char *value(const char *s) { return s && s[0] ? s : "--"; }

static lv_obj_t *label(lv_obj_t *parent, const char *text, int size,
                       int x, int y, int w, int h, lv_text_align_t align) {
    lv_obj_t *obj = espaperplay_ui_label_create(parent, text, size, align);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    return obj;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, bool border) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, w, h);
    espaperplay_ui_card_style(obj);
    if (!border) lv_obj_set_style_border_width(obj, 0, 0);
    return obj;
}

static void icon(lv_obj_t *parent, const char *code, int x, int y, bool small) {
    const lv_image_dsc_t *src = small ? qweather_icon_get_small(code) : qweather_icon_get(code);
    if (!src) src = small ? qweather_icon_get_small("999") : &icon_weather_64;
    if (!src) return;
    lv_obj_t *obj = lv_image_create(parent);
    lv_image_set_src(obj, src); lv_obj_set_pos(obj, x, y);
}

static void time_hm(const char *ts, char out[8]) {
    const char *p = ts ? strchr(ts, 'T') : NULL;
    p = p ? p + 1 : ts;
    snprintf(out, 8, "%.5s", p && p[0] ? p : "--");
}

static const char *weekday(const char *date) {
    static const char *const days[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    struct tm tm = {0};
    if (sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return "--";
    tm.tm_year -= 1900; tm.tm_mon--; tm.tm_isdst = -1;
    if (mktime(&tm) == (time_t)-1) return "--";
    return days[tm.tm_wday];
}

static void weather_detail_close(void) {
    if (s_detail) lv_obj_delete(s_detail);
    s_detail = NULL;
}

static void close_cb(lv_event_t *e) { lv_event_stop_bubbling(e); weather_detail_close(); }
static void warning_open(void);
static void warning_next_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    s_warning_index = (s_warning_index + 1) % s_snapshot->warning_count;
    warning_open();
}

static lv_obj_t *detail_open(const char *title, const char *text) {
    weather_detail_close();
    /* 页面在触摸释放时打开详情；该次 LVGL 点击随后才送达。短弹窗
     * 可能不覆盖原点击位置，因此背景不绑定关闭，避免刚打开便被同一
     * 次点击关闭。使用明确的关闭按钮或 BOOT 返回。 */
    s_detail = espaperplay_ui_modal_overlay_create(true, NULL);
    const int pw = s_width - 32;
    lv_obj_t *panel = box(s_detail, 16, 50, pw, s_height - 100, true);
    lv_obj_t *heading = espaperplay_ui_label_create(panel, title, 24, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(heading, 20, 20); lv_obj_set_width(heading, pw - 40);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_WRAP);
    lv_obj_t *body = box(panel, 20, 108, pw - 40, s_height - 290, false);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_t *desc = espaperplay_ui_label_create(body, text, 20, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(desc, pw - 44);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(panel);
    int body_y = 40 + lv_obj_get_height(heading);
    int body_h = lv_obj_get_height(desc) + 8;
    int ph = body_y + body_h + 88;
    if (ph < 280) ph = 280;
    if (ph > s_height - 100) ph = s_height - 100;
    lv_obj_set_height(panel, ph); lv_obj_set_y(panel, (s_height - ph) / 2);
    lv_obj_set_y(body, body_y); lv_obj_set_height(body, ph - body_y - 80);
    espaperplay_ui_modal_button(panel, "关闭", 20, ph - 64,
        pw - 40, 44, false, close_cb, NULL);
    lv_obj_update_layout(panel);
    return panel;
}

static void warning_open(void) {
    if (!s_snapshot || s_snapshot->warning_count <= 0) return;
    const espaperplay_weather_warning_t *w = &s_snapshot->warnings[s_warning_index];
    lv_obj_t *panel = detail_open(value(w->title), value(w->text));
    if (s_snapshot->warning_count > 1) {
        char buf[40]; snprintf(buf, sizeof(buf), "下一条 %d/%d", s_warning_index + 1, s_snapshot->warning_count);
        lv_obj_set_width(lv_obj_get_child(panel, 2), (s_width - 84) / 2);
        espaperplay_ui_modal_button(panel, buf, (s_width - 32) / 2 + 2, lv_obj_get_height(panel) - 64,
            (s_width - 84) / 2, 44, true, warning_next_cb, NULL);
    }
}

static void weather_now(void) {
    const espaperplay_weather_now_t *now = &s_snapshot->now;
    char buf[192];
    snprintf(buf, sizeof(buf), "%s°", value(now->temp));
    label(s_content, buf, 96, 24, 0, s_width - 152, 130, LV_TEXT_ALIGN_LEFT);
    icon(s_content, now->icon, s_width - 104, 38, false);
    snprintf(buf, sizeof(buf), "%s  %s° / %s°", value(now->text),
        s_snapshot->daily_count ? value(s_snapshot->daily[0].temp_min) : "--",
        s_snapshot->daily_count ? value(s_snapshot->daily[0].temp_max) : "--");
    label(s_content, buf, 24, 24, 132, s_width - 48, 40, LV_TEXT_ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "体感 %s°  ·  空气质量 %s", value(now->feels_like), value(s_snapshot->air.category));
    label(s_content, buf, 16, 24, 173, s_width - 48, 28, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *summary = box(s_content, 16, 212, s_width - 32, 62, true);
    if (s_snapshot->warning_count) {
        const espaperplay_weather_warning_t *w = &s_snapshot->warnings[0];
        snprintf(buf, sizeof(buf), "%s%s预警  >", w->type_name, w->level);
        label(summary, buf, 20, 14, 14, s_width - 60, 32, LV_TEXT_ALIGN_LEFT);
    } else {
        label(summary, s_snapshot->minutely.summary[0] ? s_snapshot->minutely.summary : "暂无降水趋势数据",
            16, 14, 9, s_width - 60, 44, LV_TEXT_ALIGN_LEFT);
    }
    label(s_content, "逐小时预报", 20, 24, 296, 180, 32, LV_TEXT_ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "%s  %d/2  >", s_hour_group ? "后 12 小时" : "前 12 小时", s_hour_group + 1);
    label(s_content, buf, 14, s_width - 210, 301, 186, 24, LV_TEXT_ALIGN_RIGHT);
    int col = (s_width - 32) / 4;
    for (int i = 0; i < 4; i++) {
        int n = s_hour_group * 12 + i * 3;
        int x = 16 + i * col;
        if (n >= s_snapshot->hourly_count) {
            label(s_content, "--", 20, x, 370, col, 32, LV_TEXT_ALIGN_CENTER);
            continue;
        }
        const espaperplay_weather_hourly_t *hour = &s_snapshot->hourly[n];
        char hm[16];
        const char *hour_start = strchr(hour->fx_time, 'T');
        hour_start = hour_start ? hour_start + 1 : hour->fx_time;
        int hour_number = -1;
        if (sscanf(hour_start, "%d", &hour_number) == 1 && hour_number >= 0 && hour_number < 24)
            snprintf(hm, sizeof(hm), "%02d时", hour_number);
        else strlcpy(hm, "--", sizeof(hm));
        label(s_content, hm, 16, x, 338, col, 24, LV_TEXT_ALIGN_CENTER);
        icon(s_content, hour->icon, x + (col - 32) / 2, 369, true);
        snprintf(buf, sizeof(buf), "%s°", value(hour->temp));
        label(s_content, buf, 24, x, 408, col, 36, LV_TEXT_ALIGN_CENTER);
    }
    const char *names[] = {"湿度", "风向 / 风力", "降水", "气压", "能见度", "云量"};
    char values[6][48];
    snprintf(values[0], 48, "%s%%", value(now->humidity));
    snprintf(values[1], 48, "%s %s级", value(now->wind_dir), value(now->wind_scale));
    snprintf(values[2], 48, "%s mm", value(now->precip));
    snprintf(values[3], 48, "%s hPa", value(now->pressure));
    snprintf(values[4], 48, "%s km", value(now->vis));
    snprintf(values[5], 48, "%s%%", value(now->cloud));
    col = (s_width - 48) / 3;
    for (int i = 0; i < 6; i++) {
        lv_obj_t *card = box(s_content, 16 + (i % 3) * (col + 8), 464 + (i / 3) * 66, col, 58, true);
        label(card, names[i], 14, 8, 4, col - 16, 20, LV_TEXT_ALIGN_LEFT);
        label(card, values[i], i == 1 ? 16 : 20, 8, 27, col - 16, 28, LV_TEXT_ALIGN_LEFT);
    }
}

static void weather_forecast(void) {
    label(s_content, "未来 7 天", 20, 24, 4, 200, 32, LV_TEXT_ALIGN_LEFT);
    label(s_content, "最低 / 最高", 16, s_width - 150, 9, 126, 24, LV_TEXT_ALIGN_RIGHT);
    int row_h = (s_height - WEATHER_CONTENT_Y - 64) / 7;
    if (row_h < 72) row_h = 72;
    for (int i = 0; i < 7; i++) {
        lv_obj_t *row = box(s_content, 16, 52 + i * row_h, s_width - 32, row_h, true);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        if (i >= s_snapshot->daily_count) {
            label(row, "暂无预报", 16, 8, 22, s_width - 48, 24, LV_TEXT_ALIGN_LEFT);
            continue;
        }
        const espaperplay_weather_daily_t *day = &s_snapshot->daily[i];
        label(row, i == 0 ? "今天" : i == 1 ? "明天" : weekday(day->fx_date), 20, 8, 10, 70, 28, LV_TEXT_ALIGN_LEFT);
        label(row, strlen(day->fx_date) >= 10 ? day->fx_date + 5 : "--", 14, 8, 40, 70, 22, LV_TEXT_ALIGN_LEFT);
        icon(row, day->icon_day, 88, (row_h - 32) / 2, true);
        label(row, value(day->text_day), 16, 138, (row_h - 24) / 2, s_width - 322, 24, LV_TEXT_ALIGN_LEFT);
        char buf[40]; snprintf(buf, sizeof(buf), "%s° / %s°", value(day->temp_min), value(day->temp_max));
        label(row, buf, 20, s_width - 176, (row_h - 30) / 2, 128, 32, LV_TEXT_ALIGN_RIGHT);
    }
}

static void weather_life(void) {
    const espaperplay_weather_astronomy_t *a = &s_snapshot->astronomy;
    const espaperplay_weather_daily_t *d = &s_snapshot->daily[0];
    char rise[8], set[8], buf[128];
    const char *sunrise = a->sunrise[0] ? a->sunrise : d->sunrise;
    const char *sunset = a->sunset[0] ? a->sunset : d->sunset;
    time_hm(sunrise, rise); time_hm(sunset, set);
    lv_obj_t *sun = box(s_content, 16, 8, s_width - 32, 120, true);
    icon(sun, "100", 18, 26, false);
    label(sun, "日出与日落", 20, 108, 14, s_width - 164, 32, LV_TEXT_ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "日出 %s     日落 %s", rise, set);
    label(sun, buf, 16, 108, 65, s_width - 160, 28, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *moon = box(s_content, 16, 140, s_width - 32, 136, true);
    /* 固定的月亮标识不冒充当前月相，也不与轨迹线相交。 */
    icon(moon, "150", 18, 36, false);
    const char *phase = a->moon_phase[0] ? a->moon_phase : d->moon_phase;
    snprintf(buf, sizeof(buf), "月亮 · %s", value(phase));
    label(moon, buf, 20, 108, 14, s_width - 160, 32, LV_TEXT_ALIGN_LEFT);
    /* 空时间代表当天没有该天象；不把前一天月出误标为今天。 */
    time_hm(a->moonrise[0] ? a->moonrise : d->moonrise, rise);
    time_hm(a->moonset[0] ? a->moonset : d->moonset, set);
    snprintf(buf, sizeof(buf), "月出 %s     月落 %s", rise, set);
    label(moon, buf, 16, 108, 58, s_width - 160, 28, LV_TEXT_ALIGN_LEFT);
    label(moon, "-- 表示今日无该天象或暂无数据", 14, 108, 97, s_width - 160, 24, LV_TEXT_ALIGN_LEFT);

    label(s_content, "生活指数", 20, 24, 300, 180, 32, LV_TEXT_ALIGN_LEFT);
    int groups = (s_snapshot->indices_count + 3) / 4;
    if (groups > 1) {
        snprintf(buf, sizeof(buf), "%d/%d  更多 >", s_index_group + 1, groups);
        label(s_content, buf, 14, s_width - 180, 304, 156, 24, LV_TEXT_ALIGN_RIGHT);
    }
    int col = (s_width - 44) / 2;
    for (int i = 0; i < 4; i++) {
        int n = s_index_group * 4 + i;
        lv_obj_t *card = box(s_content, 16 + (i % 2) * (col + 12), 350 + (i / 2) * 116, col, 104, true);
        if (n >= s_snapshot->indices_count) {
            label(card, "暂无数据", 16, 14, 38, col - 28, 28, LV_TEXT_ALIGN_LEFT);
            continue;
        }
        const espaperplay_weather_indices_t *idx = &s_snapshot->indices[n];
        label(card, value(idx->name), 16, 14, 12, col - 28, 26, LV_TEXT_ALIGN_LEFT);
        label(card, value(idx->category), 24, 14, 49, col - 42, 40, LV_TEXT_ALIGN_LEFT);
        espaperplay_ui_chevron_create(card, col - 30, 62, false);
    }
}

static void weather_render(void) {
    if (s_content) lv_obj_delete(s_content);
    s_content = box(lv_screen_active(), 0, WEATHER_CONTENT_Y, s_width,
                    s_height - WEATHER_CONTENT_Y, false);
    /* 短屏允许纵向查看内容，竖屏 480×800 的全部关键内容一屏呈现。 */
    if (s_height < 780) {
        lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
    }
    for (int i = 0; i < WEATHER_TABS; i++) {
        lv_obj_set_style_border_width(s_tabs[i], i == s_page ? 3 : 0, 0);
    }
    if (!s_valid) {
        icon(s_content, "999", (s_width - 64) / 2, 100, false);
        label(s_content, s_hint, 20, 32, 200, s_width - 64, 112, LV_TEXT_ALIGN_CENTER);
    } else if (s_page == 0) weather_now();
    else if (s_page == 1) weather_forecast();
    else weather_life();
}

static void weather_refresh(void) {
    if (s_detail) return; /* 保持正在阅读的预警/指数详情稳定。 */
    if (!s_snapshot) return;
    if (espaperplay_weather_get_snapshot(s_snapshot) != ESP_OK || !s_snapshot->valid) {
        espaperplay_weather_status_t status = {0};
        espaperplay_weather_get_status(&status);
        const char *hint = status.configured ? "正在获取天气数据…" : "请在 Web 管理页配置天气服务";
        bool changed = s_valid || strcmp(s_hint, hint);
        s_valid = false; strlcpy(s_hint, hint, sizeof(s_hint));
        if (s_retry_count++ % 30 == 0 && status.configured) espaperplay_weather_request_refresh();
        if (s_timer) lv_timer_set_period(s_timer, 1000);
        if (changed) weather_render();
        return;
    }
    if (s_timer) lv_timer_set_period(s_timer, WEATHER_PERIOD_MS);
    if (espaperplay_weather_is_refresh_due()) espaperplay_weather_request_refresh();
    bool changed = !s_valid || strcmp(s_update, s_snapshot->update_time);
    s_valid = true; s_retry_count = 0;
    if (!changed) return;
    strlcpy(s_update, s_snapshot->update_time, sizeof(s_update));
    /* 更新批次变化时才写 DOT 标签，避免标签省略后的文本触发无谓重绘。 */
    lv_label_set_text(s_location, value(s_snapshot->location_name));
    char hm[8], buf[48]; time_hm(s_update, hm);
    /* 服务 update_time 也可能使用 YYYY-MM-DD HH:MM。 */
    if (!strchr(s_update, 'T') && strlen(s_update) >= 16) snprintf(hm, sizeof(hm), "%.5s", s_update + 11);
    snprintf(buf, sizeof(buf), "%s 更新", hm);
    lv_label_set_text(s_updated, buf);
    if (s_index_group * 4 >= s_snapshot->indices_count) s_index_group = 0;
    weather_render();
}

static void timer_cb(lv_timer_t *timer) { (void)timer; weather_refresh(); }

static void weather_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    int32_t w, h; espaperplay_ui_screen_size(&w, &h); s_width = w; s_height = h;
    s_content = s_detail = NULL; s_page = s_hour_group = s_index_group = s_warning_index = 0;
    s_valid = s_touch_down = false; s_update[0] = s_hint[0] = '\0'; s_retry_count = 0;
    s_snapshot = calloc(1, sizeof(*s_snapshot));
    s_bar = espaperplay_ui_status_bar_create(scr, ESPAPERPLAY_UI_HEADER_H, "", false);
    espaperplay_ui_chevron_create(scr, 26, 60, true);
    s_location = label(scr, "天气", 32, 76, 48, w - 100, 48, LV_TEXT_ALIGN_LEFT);
    s_updated = label(scr, "等待更新", 14, 76, 100, w - 100, 24, LV_TEXT_ALIGN_LEFT);
    const char *names[] = {"现在", "预报", "生活"};
    for (int i = 0; i < WEATHER_TABS; i++) {
        s_tabs[i] = box(scr, 16 + i * ((w - 32) / 3), 138, (w - 32) / 3, 44, true);
        lv_obj_set_style_radius(s_tabs[i], 0, 0);
        lv_obj_set_style_border_side(s_tabs[i], LV_BORDER_SIDE_BOTTOM, 0);
        label(s_tabs[i], names[i], 20, 0, 3, (w - 32) / 3, 32, LV_TEXT_ALIGN_CENTER);
    }
    s_timer = lv_timer_create(timer_cb, WEATHER_PERIOD_MS, NULL);
    if (s_snapshot) weather_refresh();
    else { strlcpy(s_hint, "内存不足，请返回后重试", sizeof(s_hint)); weather_render(); }
}

static void weather_exit(void) {
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL; weather_detail_close();
    free(s_snapshot); s_snapshot = NULL; s_content = NULL;
}

static void weather_back(void) {
    if (s_detail) weather_detail_close();
    else if (espaperplay_ui_page_depth() > 1) espaperplay_ui_page_pop_lv();
}

static void weather_on_key(const espaperplay_input_event_t *event) {
    if (event->key_action == ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK) weather_back();
}

static void weather_on_touch(const espaperplay_input_event_t *event) {
    if (s_detail) { s_touch_down = false; return; }
    lv_point_t p; espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);
    if (event->touch_pressed) {
        if (!s_touch_down) { s_touch_down = true; s_start = p; }
        s_last = p; return;
    }
    if (!s_touch_down) return;
    s_touch_down = false;
    int dx = s_last.x - s_start.x, dy = s_last.y - s_start.y;
    int y = s_start.y - WEATHER_CONTENT_Y + (s_content ? lv_obj_get_scroll_y(s_content) : 0);
    if (abs(dx) > UI_GESTURE_EDGE_SWIPE_PX && abs(dx) > abs(dy) * UI_GESTURE_SWIPE_MIN_RATIO &&
        ((s_start.x < UI_GESTURE_EDGE_PX && dx > 0) || (s_start.x > s_width - UI_GESTURE_EDGE_PX && dx < 0))) {
        weather_back(); return;
    }
    if (abs(dx) > UI_GESTURE_SWIPE_PX && abs(dx) > abs(dy) * UI_GESTURE_SWIPE_MIN_RATIO) {
        if (s_page == 0 && y >= 296 && y < 450) s_hour_group = dx < 0 ? 1 : 0;
        else { int next = s_page + (dx < 0 ? 1 : -1); if (next < 0 || next >= WEATHER_TABS) return; s_page = next; }
        weather_render(); return;
    }
    if (abs(dx) > UI_GESTURE_CLICK_MAX_PX || abs(dy) > UI_GESTURE_CLICK_MAX_PX) return;
    if (s_start.y >= ESPAPERPLAY_UI_HEADER_H && s_start.y < 112 && s_start.x < 72) { weather_back(); return; }
    if (s_start.y >= 138 && s_start.y < 182 && s_start.x >= 16 && s_start.x < s_width - 16) {
        s_page = (s_start.x - 16) / ((s_width - 32) / 3);
        if (s_page >= WEATHER_TABS) s_page = WEATHER_TABS - 1;
        weather_render(); return;
    }
    if (!s_valid) return;
    if (s_page == 0 && y >= 212 && y < 274 && s_snapshot->warning_count) {
        s_warning_index = 0; warning_open();
    } else if (s_page == 0 && y >= 296 && y < 450) {
        s_hour_group = 1 - s_hour_group; weather_render();
    } else if (s_page == 2 && y >= 296 && y < 342 && s_snapshot->indices_count > 4) {
        s_index_group = (s_index_group + 1) % ((s_snapshot->indices_count + 3) / 4); weather_render();
    } else if (s_page == 2 && y >= 350 && y < 570) {
        int col_w = (s_width - 44) / 2;
        int row = (y - 350) / 116;
        int col = s_start.x >= 16 + col_w + 12 ? 1 : 0;
        int n = s_index_group * 4 + row * 2 + col;
        int x = 16 + col * (col_w + 12);
        if (s_start.x >= x && s_start.x < x + col_w && (y - 350) % 116 < 104 && n < s_snapshot->indices_count) {
            const espaperplay_weather_indices_t *idx = &s_snapshot->indices[n];
            char title[80]; snprintf(title, sizeof(title), "%s · %s", idx->name, idx->category);
            detail_open(title, value(idx->text));
        }
    }
}

const espaperplay_ui_page_t espaperplay_ui_page_weather = {
    weather_enter, weather_exit, weather_on_key, weather_on_touch};
