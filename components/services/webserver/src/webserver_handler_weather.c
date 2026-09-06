/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>

#include "esp_http_server.h"

#include "espaperplay_system.h"
#include "espaperplay_weather.h"
#include "webserver_internal.h"

/* ------------------------------------------------------------------ */
/* JSON 构造辅助（把快照数据转为 JSON）                                 */
/* ------------------------------------------------------------------ */

/** 从完整时间戳截取 "HH:MM"（天文接口返回 "2026-08-16T05:12+08:00"）；
 * 空值（当天无月出/月落现象）返回 "--" 占位。 */
static const char *weather_ts_hm(const char *ts, char *buf, size_t buf_size) {
    if (ts != NULL && ts[0] != '\0') {
        const char *t = strchr(ts, 'T');
        if (t != NULL && t[1] != '\0' && t[2] != '\0' && t[3] != '\0' && t[4] != '\0') {
            snprintf(buf, buf_size, "%.5s", t + 1);
            return buf;
        }
        return ts;
    }
    return "--";
}

/** 追加实时天气对象。 */
static void weather_json_add_now(cJSON *parent, const espaperplay_weather_now_t *now) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "obs_time", now->obs_time);
    cJSON_AddStringToObject(obj, "temp", now->temp);
    cJSON_AddStringToObject(obj, "feels_like", now->feels_like);
    cJSON_AddStringToObject(obj, "icon", now->icon);
    cJSON_AddStringToObject(obj, "text", now->text);
    cJSON_AddStringToObject(obj, "wind_dir", now->wind_dir);
    cJSON_AddStringToObject(obj, "wind_scale", now->wind_scale);
    cJSON_AddStringToObject(obj, "wind_speed", now->wind_speed);
    cJSON_AddStringToObject(obj, "humidity", now->humidity);
    cJSON_AddStringToObject(obj, "precip", now->precip);
    cJSON_AddStringToObject(obj, "pressure", now->pressure);
    cJSON_AddStringToObject(obj, "vis", now->vis);
    cJSON_AddStringToObject(obj, "cloud", now->cloud);
    cJSON_AddStringToObject(obj, "dew", now->dew);
    cJSON_AddItemToObject(parent, "now", obj);
}

/** 追加逐日预报数组。 */
static void weather_json_add_daily(cJSON *parent, const espaperplay_weather_snapshot_t *snap) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < snap->daily_count; i++) {
        const espaperplay_weather_daily_t *d = &snap->daily[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "date", d->fx_date);
        cJSON_AddStringToObject(obj, "temp_max", d->temp_max);
        cJSON_AddStringToObject(obj, "temp_min", d->temp_min);
        cJSON_AddStringToObject(obj, "icon_day", d->icon_day);
        cJSON_AddStringToObject(obj, "text_day", d->text_day);
        cJSON_AddStringToObject(obj, "icon_night", d->icon_night);
        cJSON_AddStringToObject(obj, "text_night", d->text_night);
        cJSON_AddStringToObject(obj, "wind_dir_day", d->wind_dir_day);
        cJSON_AddStringToObject(obj, "wind_scale_day", d->wind_scale_day);
        cJSON_AddStringToObject(obj, "humidity", d->humidity);
        cJSON_AddStringToObject(obj, "uv_index", d->uv_index);
        cJSON_AddItemToArray(arr, obj);
    }
    cJSON_AddItemToObject(parent, "daily", arr);
}

/** 追加预警数组（只含关键字段，避免响应过大）。 */
static void weather_json_add_warnings(cJSON *parent, const espaperplay_weather_snapshot_t *snap) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < snap->warning_count; i++) {
        const espaperplay_weather_warning_t *w = &snap->warnings[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", w->id);
        cJSON_AddStringToObject(obj, "title", w->title);
        cJSON_AddStringToObject(obj, "type_name", w->type_name);
        cJSON_AddStringToObject(obj, "level", w->level);
        cJSON_AddStringToObject(obj, "severity", w->severity);
        cJSON_AddStringToObject(obj, "severity_color", w->severity_color);
        cJSON_AddStringToObject(obj, "sender", w->sender);
        cJSON_AddStringToObject(obj, "pub_time", w->pub_time);
        cJSON_AddStringToObject(obj, "start_time", w->start_time);
        cJSON_AddStringToObject(obj, "end_time", w->end_time);
        cJSON_AddItemToArray(arr, obj);
    }
    cJSON_AddItemToObject(parent, "warnings", arr);
}

/* ------------------------------------------------------------------ */
/* 路由处理器                                                           */
/* ------------------------------------------------------------------ */

/** GET /api/weather —— 天气服务状态与数据快照摘要。 */
esp_err_t webserver_handle_weather_get(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    /* 快照较大（约 20KB），在堆上分配。 */
    espaperplay_weather_snapshot_t *snap = malloc(sizeof(*snap));
    if (snap == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    espaperplay_weather_status_t status;
    esp_err_t err = espaperplay_weather_get_status(&status);
    if (err != ESP_OK || espaperplay_weather_get_snapshot(snap) != ESP_OK) {
        free(snap);
        webserver_send_json_err(req, esp_err_to_name(err));
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        free(snap);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root, "configured", status.configured);
    cJSON_AddBoolToObject(root, "valid", status.valid);
    cJSON_AddBoolToObject(root, "auto_location", status.auto_location);
    cJSON_AddBoolToObject(root, "task_running", status.task_running);
    cJSON_AddNumberToObject(root, "last_api_code", status.last_api_code);
    cJSON_AddStringToObject(root, "last_error", status.last_error);
    cJSON_AddStringToObject(root, "location_id", status.location_id);
    cJSON_AddStringToObject(root, "location_name", status.location_name);
    cJSON_AddStringToObject(root, "update_time", status.update_time);
    cJSON_AddNumberToObject(root, "hourly_count", snap->hourly_count);
    cJSON_AddNumberToObject(root, "warning_count", snap->warning_count);
    cJSON_AddNumberToObject(root, "indices_count", snap->indices_count);

    if (snap->valid) {
        weather_json_add_now(root, &snap->now);
        weather_json_add_daily(root, snap);
        weather_json_add_warnings(root, snap);

        cJSON *minutely = cJSON_CreateObject();
        cJSON_AddStringToObject(minutely, "summary", snap->minutely.summary);
        cJSON_AddNumberToObject(minutely, "count", snap->minutely.count);
        cJSON_AddItemToObject(root, "minutely", minutely);

        cJSON *air = cJSON_CreateObject();
        cJSON_AddStringToObject(air, "aqi", snap->air.aqi);
        cJSON_AddStringToObject(air, "level", snap->air.level);
        cJSON_AddStringToObject(air, "category", snap->air.category);
        cJSON_AddStringToObject(air, "primary", snap->air.primary);
        cJSON_AddItemToObject(root, "air", air);

        cJSON *astro = cJSON_CreateObject();
        char hm[8];
        cJSON_AddStringToObject(astro, "date", snap->astronomy.date);
        cJSON_AddStringToObject(astro, "sunrise",
                                weather_ts_hm(snap->astronomy.sunrise, hm, sizeof(hm)));
        cJSON_AddStringToObject(astro, "sunset",
                                weather_ts_hm(snap->astronomy.sunset, hm, sizeof(hm)));
        cJSON_AddStringToObject(astro, "moonrise",
                                weather_ts_hm(snap->astronomy.moonrise, hm, sizeof(hm)));
        cJSON_AddStringToObject(astro, "moonset",
                                weather_ts_hm(snap->astronomy.moonset, hm, sizeof(hm)));
        cJSON_AddStringToObject(astro, "moonrise_prev",
                                weather_ts_hm(snap->astronomy.moonrise_prev, hm, sizeof(hm)));
        cJSON_AddStringToObject(astro, "moonrise_next",
                                weather_ts_hm(snap->astronomy.moonrise_next, hm, sizeof(hm)));
        cJSON_AddStringToObject(astro, "moonset_next",
                                weather_ts_hm(snap->astronomy.moonset_next, hm, sizeof(hm)));
        cJSON_AddStringToObject(astro, "moon_phase", snap->astronomy.moon_phase);
        cJSON_AddItemToObject(root, "astronomy", astro);
    }

    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    free(snap);
    return ESP_OK;
}

/** POST /api/weather/refresh —— 请求立即刷新天气数据（异步执行）。 */
esp_err_t webserver_handle_weather_refresh_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    if (espaperplay_system_get_config()->weather_api_key[0] == '\0') {
        webserver_send_json_err(req, "请先配置和风天气 API Key");
        return ESP_FAIL;
    }

    espaperplay_weather_request_refresh();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "queued", true);
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}
