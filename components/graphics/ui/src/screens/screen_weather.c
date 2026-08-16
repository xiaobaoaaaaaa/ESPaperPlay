/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "espaperplay_input.h"
#include "espaperplay_ui.h"
#include "espaperplay_weather.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 天气页（和风天气数据展示）
 * ====================================================================
 *
 * 展示 weather 服务内存快照（由后台任务周期刷新）：实时天气、体感 /
 * 湿度 / 风、未来 3 天预报、空气质量、日出日落、月升月落、2 小时分钟级
 * 降水摘要与气象灾害预警。
 *
 * 注：内置 LVGL 字体仅覆盖拉丁字符，中文天气描述（如 "晴"）无法渲染，
 * 因此把 QWeather 图标代码 / 预警类型代码映射为英文标签展示；数值字段
 * （温度、湿度、AQI 等）原样显示。
 *
 * 布局使用百分比（相对屏幕尺寸）：标题（位置 + 更新时间）、左侧实时
 * 天气大字、右侧空气 / 天文、底部 3 日预报三列、预警与降水摘要行。
 * 单击返回上一页。
 */

#define WEATHER_UI_PERIOD_MS 30000 /* 页面定时器刷新周期 */

/** 数据是否变化才更新标签（EPD 上避免无谓刷新）。 */
static char s_last_update[32] = "";

static lv_obj_t *s_title_label = NULL;    /*!< 位置 + 更新时间 */
static lv_obj_t *s_temp_label = NULL;     /*!< 实时温度（48 号大字） */
static lv_obj_t *s_cond_label = NULL;     /*!< 天气状况（图标代码映射英文） */
static lv_obj_t *s_detail_label = NULL;   /*!< 体感 / 湿度 / 风 / 降水 */
static lv_obj_t *s_air_label = NULL;      /*!< 空气质量 */
static lv_obj_t *s_astro_label = NULL;    /*!< 日出日落 + 月升月落 */
static lv_obj_t *s_minutely_label = NULL; /*!< 分钟级降水摘要 */
static lv_obj_t *s_warn_label = NULL;     /*!< 气象灾害预警 */
static lv_obj_t *s_daily_labels[3] = {NULL, NULL, NULL}; /*!< 未来 3 天 */
static lv_obj_t *s_status_label = NULL;   /*!< 状态 / 错误提示 */
static lv_timer_t *s_weather_timer = NULL; /*!< 页面定时器（退出时删除） */

/* ------------------------------------------------------------------ */
/* QWeather 代码 -> 英文标签映射                                        */
/* ------------------------------------------------------------------ */

/** 天气图标代码 -> 英文描述（覆盖常见代码，未命中返回 NULL）。 */
static const char *weather_icon_text(const char *icon) {
    if (icon == NULL || icon[0] == '\0') {
        return NULL;
    }
    const int code = atoi(icon);
    switch (code) {
    case 100:
        return "Sunny";
    case 101:
        return "Cloudy";
    case 102:
        return "Few clouds";
    case 103:
        return "Partly cloudy";
    case 104:
        return "Overcast";
    case 150:
        return "Clear night";
    case 151:
        return "Cloudy night";
    case 152:
        return "Partly cloudy night";
    case 153:
        return "Overcast night";
    case 300:
        return "Shower";
    case 301:
        return "Heavy shower";
    case 302:
        return "Thundershower";
    case 303:
        return "Heavy thundershower";
    case 304:
        return "Thundershower w/ hail";
    case 305:
        return "Light rain";
    case 306:
        return "Moderate rain";
    case 307:
        return "Heavy rain";
    case 308:
        return "Extreme rain";
    case 309:
        return "Drizzle";
    case 310:
        return "Rainstorm";
    case 311:
        return "Severe rainstorm";
    case 312:
        return "Extreme rainstorm";
    case 313:
        return "Freezing rain";
    case 314:
        return "Light-moderate rain";
    case 315:
        return "Moderate-heavy rain";
    case 316:
        return "Heavy rainstorm";
    case 317:
        return "Rainstorm-severe";
    case 318:
        return "Severe-extreme";
    case 350:
    case 399:
        return "Rain";
    case 400:
        return "Light snow";
    case 401:
        return "Moderate snow";
    case 402:
        return "Heavy snow";
    case 403:
        return "Blizzard";
    case 404:
        return "Sleet";
    case 405:
        return "Rain & snow";
    case 406:
        return "Snow shower";
    case 407:
        return "Snow flurry";
    case 408:
        return "Light-moderate snow";
    case 409:
        return "Moderate-heavy snow";
    case 410:
        return "Heavy snowstorm";
    case 456:
    case 504:
        return "Dust";
    case 457:
    case 503:
        return "Blowing sand";
    case 499:
        return "Snow";
    case 500:
        return "Mist";
    case 501:
    case 509:
        return "Fog";
    case 502:
    case 511:
        return "Haze";
    case 507:
        return "Sandstorm";
    case 508:
        return "Severe sandstorm";
    case 510:
        return "Dense fog";
    case 512:
        return "Heavy haze";
    case 513:
        return "Severe haze";
    case 514:
        return "Thick fog";
    case 900:
        return "Hot";
    case 901:
        return "Cold";
    default:
        return NULL;
    }
}

/** 预警类型代码 -> 英文描述（预警类型见 QWeather 文档）。 */
static const char *weather_warning_type_text(const char *type) {
    if (type == NULL || type[0] == '\0') {
        return NULL;
    }
    const int code = atoi(type);
    switch (code) {
    case 1:
        return "Typhoon";
    case 2:
        return "Rainstorm";
    case 3:
        return "Snowstorm";
    case 4:
        return "Cold wave";
    case 5:
        return "Gale";
    case 6:
        return "Sandstorm";
    case 7:
        return "Heat";
    case 8:
        return "Drought";
    case 9:
        return "Thunder";
    case 10:
        return "Hail";
    case 11:
        return "Frost";
    case 12:
        return "Fog";
    case 13:
        return "Haze";
    case 14:
        return "Icy road";
    case 15:
        return "Dry hot wind";
    case 16:
        return "Thunder gale";
    case 17:
        return "Severe convection";
    case 19:
        return "Geohazard";
    case 20:
        return "Forest fire";
    default:
        return NULL;
    }
}

/** 解析 "YYYY-MM-DD" 为星期几英文缩写（如 "Fri"）。 */
static void weather_weekday(const char *date, char *out, size_t out_size) {
    struct tm tm = {0};
    if (date == NULL || sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) {
        strlcpy(out, "-", out_size);
        return;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    strftime(out, out_size, "%a", &tm_local);
}

/**
 * @brief 从完整时间戳中截取 "HH:MM"（天文接口返回 "2026-08-16T05:12+08:00"）。
 * 已是短格式时原样返回。
 */
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
/* 页面构建与刷新                                                       */
/* ------------------------------------------------------------------ */

/** 刷新全部天气标签（LVGL 线程内；数据未变化时跳过）。 */
static void weather_refresh(void) {
    /* 快照较大（约 20KB），在堆上分配。 */
    espaperplay_weather_snapshot_t *snap = malloc(sizeof(*snap));
    if (snap == NULL) {
        ESP_LOGW(TAG, "weather: snapshot alloc failed");
        return;
    }
    if (espaperplay_weather_get_snapshot(snap) != ESP_OK || !snap->valid) {
        lv_label_set_text(s_title_label, "Weather");
        lv_label_set_text(s_temp_label, "--");
        lv_label_set_text(s_cond_label, "not available");
        lv_label_set_text(s_status_label,
                          "configure API key on the web page\nclick: back");
        free(snap);
        return;
    }

    /* 更新时变化才刷新标签（EPD 局部刷新省电）。 */
    if (strcmp(s_last_update, snap->update_time) == 0) {
        free(snap);
        return;
    }
    strlcpy(s_last_update, snap->update_time, sizeof(s_last_update));

    const espaperplay_weather_now_t *now = &snap->now;
    char buf[256];

    /* 标题：位置 + 更新时间。 */
    snprintf(buf, sizeof(buf), "%s  (%s)", snap->location_name, snap->update_time);
    lv_label_set_text(s_title_label, buf);

    /* 实时天气：大字号温度 + 状况。 */
    snprintf(buf, sizeof(buf), "%s°C", now->temp[0] ? now->temp : "--");
    lv_label_set_text(s_temp_label, buf);

    const char *cond = weather_icon_text(now->icon);
    lv_label_set_text(s_cond_label, cond != NULL ? cond : (now->text[0] ? "Weather" : "-"));

    /* 细节行：体感 / 湿度 / 风 / 降水 / 气压。 */
    snprintf(buf, sizeof(buf), "feels %s°C   hum %s%%   wind %s %s   precip %s mm",
             now->feels_like[0] ? now->feels_like : "-", now->humidity[0] ? now->humidity : "-",
             now->wind_dir[0] ? now->wind_dir : "-", now->wind_scale[0] ? now->wind_scale : "-",
             now->precip[0] ? now->precip : "-");
    lv_label_set_text(s_detail_label, buf);

    /* 空气质量。 */
    if (snap->air.aqi[0] != '\0') {
        snprintf(buf, sizeof(buf), "Air   AQI %s  %s", snap->air.aqi, snap->air.category);
    } else {
        snprintf(buf, sizeof(buf), "Air   n/a");
    }
    lv_label_set_text(s_air_label, buf);

    /* 天文：日出日落 + 月升月落（时间戳截取 HH:MM）。 */
    {
        char sun_r[6], sun_s[6], moon_r[6], moon_s[6];
        snprintf(buf, sizeof(buf), "Sun %s - %s   Moon %s - %s",
                 weather_time_hm(snap->astronomy.sunrise, sun_r, sizeof(sun_r)),
                 weather_time_hm(snap->astronomy.sunset, sun_s, sizeof(sun_s)),
                 weather_time_hm(snap->astronomy.moonrise, moon_r, sizeof(moon_r)),
                 weather_time_hm(snap->astronomy.moonset, moon_s, sizeof(moon_s)));
        lv_label_set_text(s_astro_label, buf);
    }

    /* 分钟级降水：2 小时内最大降水强度。 */
    {
        float max_precip = 0.0f;
        for (int i = 0; i < snap->minutely.count; i++) {
            float v = (float)atof(snap->minutely.precip[i]);
            if (v > max_precip) {
                max_precip = v;
            }
        }
        if (snap->minutely.count > 0 && max_precip >= 0.05f) {
            snprintf(buf, sizeof(buf), "2h precip   max %.1f mm/h", (double)max_precip);
        } else if (snap->minutely.count > 0) {
            snprintf(buf, sizeof(buf), "2h precip   none");
        } else {
            snprintf(buf, sizeof(buf), "2h precip   n/a");
        }
        lv_label_set_text(s_minutely_label, buf);
    }

    /* 气象灾害预警（最多 2 条摘要）。 */
    if (snap->warning_count > 0) {
        const espaperplay_weather_warning_t *w = &snap->warnings[0];
        const char *wtype = weather_warning_type_text(w->type);
        snprintf(buf, sizeof(buf), "Alert  %s %s%s", wtype != NULL ? wtype : "Weather",
                 w->level[0] ? w->level : "",
                 snap->warning_count > 1 ? "  (+more)" : "");
    } else {
        snprintf(buf, sizeof(buf), "Alert   none");
    }
    lv_label_set_text(s_warn_label, buf);

    /* 未来 3 天预报。 */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *label = s_daily_labels[i];
        if (i >= snap->daily_count || label == NULL) {
            continue;
        }
        const espaperplay_weather_daily_t *d = &snap->daily[i];
        char wd[8];
        weather_weekday(d->fx_date, wd, sizeof(wd));
        const char *dcond = weather_icon_text(d->icon_day);
        snprintf(buf, sizeof(buf), "%s\n%s\n%s\n%s° / %s°", wd,
                 dcond != NULL ? dcond : "Weather", d->fx_date,
                 d->temp_max[0] ? d->temp_max : "-", d->temp_min[0] ? d->temp_min : "-");
        lv_label_set_text(label, buf);
    }

    /* 状态行。 */
    snprintf(buf, sizeof(buf), "updated %s   click: back", snap->update_time);
    lv_label_set_text(s_status_label, buf);

    ESP_LOGI(TAG, "weather screen refreshed: %s %s°C, alerts %d",
             snap->location_name, now->temp, snap->warning_count);
    free(snap);
}

/** 周期刷新（LVGL 线程内，lv_timer 驱动）。 */
static void weather_timer_cb(lv_timer_t *timer) {
    (void)timer;
    weather_refresh();
}

/** 天气页构建（页面 enter：屏幕已由页面栈清空）。 */
static void weather_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    /* 标题：位置 + 更新时间（顶部居中）。 */
    s_title_label = lv_label_create(scr);
    lv_label_set_text(s_title_label, "Weather");
    lv_obj_set_style_text_color(s_title_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_24, 0);
    lv_obj_set_width(s_title_label, lv_pct(96));
    lv_obj_set_pos(s_title_label, lv_pct(2), lv_pct(4));
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);

    /* 左列：实时天气（大字号温度 + 状况 + 细节）。 */
    s_temp_label = lv_label_create(scr);
    lv_label_set_text(s_temp_label, "--");
    lv_obj_set_style_text_color(s_temp_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_temp_label, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(s_temp_label, lv_pct(5), lv_pct(18));

    s_cond_label = lv_label_create(scr);
    lv_label_set_text(s_cond_label, "not available");
    lv_obj_set_style_text_color(s_cond_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_cond_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_cond_label, lv_pct(5), lv_pct(40));

    s_detail_label = lv_label_create(scr);
    lv_label_set_text(s_detail_label, "");
    lv_obj_set_style_text_color(s_detail_label, lv_color_black(), 0);
    lv_obj_set_width(s_detail_label, lv_pct(46));
    lv_obj_set_pos(s_detail_label, lv_pct(5), lv_pct(50));

    /* 右列：空气质量 / 天文 / 分钟级降水 / 预警。 */
    s_air_label = lv_label_create(scr);
    lv_label_set_text(s_air_label, "");
    lv_obj_set_style_text_color(s_air_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_air_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_air_label, lv_pct(52), lv_pct(18));

    s_astro_label = lv_label_create(scr);
    lv_label_set_text(s_astro_label, "");
    lv_obj_set_style_text_color(s_astro_label, lv_color_black(), 0);
    lv_obj_set_width(s_astro_label, lv_pct(46));
    lv_obj_set_pos(s_astro_label, lv_pct(52), lv_pct(30));

    s_minutely_label = lv_label_create(scr);
    lv_label_set_text(s_minutely_label, "");
    lv_obj_set_style_text_color(s_minutely_label, lv_color_black(), 0);
    lv_obj_set_pos(s_minutely_label, lv_pct(52), lv_pct(42));

    s_warn_label = lv_label_create(scr);
    lv_label_set_text(s_warn_label, "");
    lv_obj_set_style_text_color(s_warn_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_warn_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_warn_label, lv_pct(52), lv_pct(52));

    /* 底部：未来 3 天预报三列。 */
    static const int daily_x[] = {4, 38, 72};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *label = lv_label_create(scr);
        lv_label_set_text(label, "-");
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_obj_set_width(label, lv_pct(26));
        lv_obj_set_pos(label, lv_pct(daily_x[i]), lv_pct(64));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        s_daily_labels[i] = label;
    }

    /* 状态行：更新时间 + 返回提示。 */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "click: back");
    lv_obj_set_style_text_color(s_status_label, lv_color_black(), 0);
    lv_obj_set_width(s_status_label, lv_pct(96));
    lv_obj_set_pos(s_status_label, lv_pct(2), lv_pct(92));
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);

    s_last_update[0] = '\0';
    s_weather_timer = lv_timer_create(weather_timer_cb, WEATHER_UI_PERIOD_MS, NULL);
    /* 立即刷新一次：进入页面即显示当前快照，不等定时器。 */
    weather_refresh();

    ESP_LOGI(TAG, "weather screen entered");
}

/** 天气页退出（页面 exit：删除定时器，避免离开页面后仍刷新屏幕）。 */
static void weather_exit(void) {
    if (s_weather_timer != NULL) {
        lv_timer_delete(s_weather_timer);
        s_weather_timer = NULL;
    }
    s_title_label = NULL;
    s_temp_label = NULL;
    s_cond_label = NULL;
    s_detail_label = NULL;
    s_air_label = NULL;
    s_astro_label = NULL;
    s_minutely_label = NULL;
    s_warn_label = NULL;
    s_daily_labels[0] = NULL;
    s_daily_labels[1] = NULL;
    s_daily_labels[2] = NULL;
    s_status_label = NULL;
    ESP_LOGI(TAG, "weather screen exited");
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
                                                           weather_on_key, NULL};
