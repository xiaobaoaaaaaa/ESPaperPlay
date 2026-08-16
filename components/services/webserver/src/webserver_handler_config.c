/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include "espaperplay_epd.h"
#include "espaperplay_gui.h"
#include "espaperplay_system.h"
#include "espaperplay_weather.h"
#include "espaperplay_wifi.h"
#include "webserver_internal.h"

static const char *TAG = "ESPaperPlay_WEB_CFG";

/* POST 表单请求体的最大字节数（超过直接拒绝，防内存放大）。 */
#define ESPAPERPLAY_WEB_FORM_BUF_SIZE 1024

/* ------------------------------------------------------------------ */
/* 配置域路由处理器                                                     */
/* ------------------------------------------------------------------ */

/** GET /api/config —— 返回当前系统配置。 */
esp_err_t webserver_handle_config_get(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "wifi_mode",
                            cfg->wifi_mode == ESPAPERPLAY_WIFI_MODE_AP ? "ap" : "sta");
    cJSON_AddStringToObject(root, "sta_ssid", cfg->sta_ssid);
    cJSON_AddStringToObject(root, "sta_password", cfg->sta_password);
    cJSON_AddStringToObject(root, "ap_ssid", cfg->ap_ssid);
    cJSON_AddStringToObject(root, "ap_password", cfg->ap_password);
    /* 屏幕空闲自动睡眠超时（秒，0=关闭）。 */
    cJSON_AddNumberToObject(root, "epd_idle_sleep_timeout_s",
                            (double)(cfg->epd_idle_sleep_timeout_ms / 1000));
    /* 连续大面积局刷后强制全刷阈值（0=禁用，只局刷）。 */
    cJSON_AddNumberToObject(root, "gui_full_force_after", (double)cfg->gui_full_force_after);
    /* 和风天气：API Key 不回传明文，仅报告是否已配置；位置与 API Host 非机密，原样返回。 */
    cJSON_AddBoolToObject(root, "weather_api_key_set", cfg->weather_api_key[0] != '\0');
    cJSON_AddStringToObject(root, "weather_location", cfg->weather_location);
    cJSON_AddStringToObject(root, "weather_api_host", cfg->weather_api_host);

    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** POST /api/config —— 更新系统配置并重新应用 WiFi。 */
esp_err_t webserver_handle_config_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    /* 读取表单编码的请求体。 */
    int total = req->content_len;
    if (total <= 0 || total >= ESPAPERPLAY_WEB_FORM_BUF_SIZE) {
        webserver_send_json_err(req, "请求体过大或为空");
        return ESP_FAIL;
    }
    char *body = malloc((size_t)total + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, (size_t)(total - received));
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "读取请求体失败");
            free(body);
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    /* 解析字段。仅应用请求体中*出现*的字段（部分更新）：完整表单 = 全量，
     * 屏幕设置表单只提交其自身字段。webserver_form_get_field 返回该字段是否出现。 */
    char wifi_mode_str[8] = {0};
    char sta_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN] = {0};
    char sta_password[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN] = {0};
    char ap_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN] = {0};
    char ap_password[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN] = {0};
    char epd_idle_sleep_s[16] = {0}; /* 屏幕空闲自动睡眠超时（秒） */
    char gui_force_after_s[8] = {0}; /* 连续大面积局刷后强制全刷阈值（0=禁用） */
    const bool has_wifi_mode =
        webserver_form_get_field(body, "wifi_mode", wifi_mode_str, sizeof(wifi_mode_str));
    const bool has_sta_ssid =
        webserver_form_get_field(body, "sta_ssid", sta_ssid, sizeof(sta_ssid));
    const bool has_sta_pass =
        webserver_form_get_field(body, "sta_password", sta_password, sizeof(sta_password));
    const bool has_ap_ssid = webserver_form_get_field(body, "ap_ssid", ap_ssid, sizeof(ap_ssid));
    const bool has_ap_pass =
        webserver_form_get_field(body, "ap_password", ap_password, sizeof(ap_password));
    const bool has_epd_idle = webserver_form_get_field(body, "epd_idle_sleep_timeout_s",
                                                       epd_idle_sleep_s, sizeof(epd_idle_sleep_s));
    const bool has_gui_force = webserver_form_get_field(
        body, "gui_full_force_after", gui_force_after_s, sizeof(gui_force_after_s));
    const bool clear_sta_password = webserver_form_get_flag(body, "clear_sta_password");
    const bool clear_ap_password = webserver_form_get_flag(body, "clear_ap_password");
    /* 和风天气字段（API Key 留空且未勾选清除 = 保持不变；位置留空 = 自动定位；
     * API Host 留空 = 使用公共地址）。 */
    char weather_api_key[ESPAPERPLAY_SYSTEM_WEATHER_KEY_MAX_LEN] = {0};
    char weather_location[ESPAPERPLAY_SYSTEM_WEATHER_LOC_MAX_LEN] = {0};
    char weather_api_host[ESPAPERPLAY_SYSTEM_WEATHER_HOST_MAX_LEN] = {0};
    const bool has_weather_key =
        webserver_form_get_field(body, "weather_api_key", weather_api_key,
                                 sizeof(weather_api_key));
    const bool has_weather_loc = webserver_form_get_field(
        body, "weather_location", weather_location, sizeof(weather_location));
    const bool has_weather_host = webserver_form_get_field(
        body, "weather_api_host", weather_api_host, sizeof(weather_api_host));
    const bool clear_weather_key = webserver_form_get_flag(body, "clear_weather_api_key");
    const bool clear_weather_loc = webserver_form_get_flag(body, "clear_weather_location");
    const bool clear_weather_host = webserver_form_get_flag(body, "clear_weather_api_host");
    free(body);

    const espaperplay_system_config_t *cur = espaperplay_system_get_config();
    esp_err_t err = ESP_OK;

    /* 工作模式：仅当字段出现时校验并切换（缺席 = 保持当前）。 */
    espaperplay_wifi_mode_t mode = cur->wifi_mode;
    if (has_wifi_mode) {
        if (strcmp(wifi_mode_str, "sta") == 0) {
            mode = ESPAPERPLAY_WIFI_MODE_STA;
        } else if (strcmp(wifi_mode_str, "ap") == 0) {
            mode = ESPAPERPLAY_WIFI_MODE_AP;
        } else {
            webserver_send_json_err(req, "无效的 wifi_mode");
            return ESP_FAIL;
        }
    }

    /* 密码语义（字段出现时）：输入非空 → 设置新值；输入为空且勾选"清除" → 置空；
     * 否则保留当前值。缺失字段 = 保留当前值。 */
    const char *new_sta_pass = cur->sta_password;
    const char *new_ap_pass = cur->ap_password;
    if (has_sta_pass) {
        new_sta_pass =
            (sta_password[0] == '\0' && !clear_sta_password) ? cur->sta_password : sta_password;
    }
    if (has_ap_pass) {
        new_ap_pass =
            (ap_password[0] == '\0' && !clear_ap_password) ? cur->ap_password : ap_password;
    }

    /* 校验：出现且为目标模式时，SSID 不能为空。 */
    if (has_sta_ssid && mode == ESPAPERPLAY_WIFI_MODE_STA && sta_ssid[0] == '\0') {
        webserver_send_json_err(req, "STA SSID 不能为空");
        return ESP_FAIL;
    }
    if (has_ap_ssid && mode == ESPAPERPLAY_WIFI_MODE_AP && ap_ssid[0] == '\0') {
        webserver_send_json_err(req, "AP SSID 不能为空");
        return ESP_FAIL;
    }

    /* 依次应用并持久化到 NVS（SSID/密码成对应用，缺失的一侧取当前值）。
     * wifi_changed：WiFi 相关字段确有变化时才需要重启 WiFi——屏幕设置等
     * 非网络字段的保存不应打断当前连接。 */
    bool wifi_changed = false;
    if (err == ESP_OK && has_wifi_mode && mode != cur->wifi_mode) {
        err = espaperplay_system_set_wifi_mode(mode);
        if (err == ESP_OK) {
            wifi_changed = true;
        }
    }
    if (err == ESP_OK && (has_sta_ssid || has_sta_pass) &&
        (strcmp(has_sta_ssid ? sta_ssid : cur->sta_ssid, cur->sta_ssid) != 0 ||
         strcmp(new_sta_pass, cur->sta_password) != 0)) {
        err = espaperplay_system_set_sta_credentials(has_sta_ssid ? sta_ssid : cur->sta_ssid,
                                                     new_sta_pass);
        if (err == ESP_OK) {
            wifi_changed = true;
        }
    }
    if (err == ESP_OK && (has_ap_ssid || has_ap_pass) &&
        (strcmp(has_ap_ssid ? ap_ssid : cur->ap_ssid, cur->ap_ssid) != 0 ||
         strcmp(new_ap_pass, cur->ap_password) != 0)) {
        err = espaperplay_system_set_ap_credentials(has_ap_ssid ? ap_ssid : cur->ap_ssid,
                                                    new_ap_pass);
        if (err == ESP_OK) {
            wifi_changed = true;
        }
    }
    /* 屏幕空闲自动睡眠超时（秒 -> 毫秒，0=关闭）；字段缺失 = 保持不变。 */
    if (err == ESP_OK && has_epd_idle) {
        char *end = NULL;
        long secs = strtol(epd_idle_sleep_s, &end, 10);
        if (end == epd_idle_sleep_s || *end != '\0' || secs < 0 || secs > 86400) {
            webserver_send_json_err(req, "无效的屏幕睡眠超时（0-86400 秒）");
            return ESP_FAIL;
        }
        const uint32_t ms = (uint32_t)secs * 1000;
        err = espaperplay_system_set_epd_idle_sleep_timeout_ms(ms);
        if (err == ESP_OK) {
            /* 立即应用到驱动（不必等重启）。 */
            err = espaperplay_epd_set_idle_sleep_timeout_ms(ms);
        }
    }
    /* 连续大面积局刷后强制全刷阈值（0=禁用，0-255）；字段缺失 = 保持不变。 */
    if (err == ESP_OK && has_gui_force) {
        char *end = NULL;
        long n = strtol(gui_force_after_s, &end, 10);
        if (end == gui_force_after_s || *end != '\0' || n < 0 || n > 255) {
            webserver_send_json_err(req, "无效的全刷阈值（0-255，0=禁用）");
            return ESP_FAIL;
        }
        err = espaperplay_system_set_gui_full_force_after((uint32_t)n);
        if (err == ESP_OK) {
            /* 立即应用到渲染后端（不必等重启）。 */
            err = espaperplay_gui_set_full_force_after((uint32_t)n);
        }
    }
    /* 和风天气：字段出现时应用。API Key 语义与密码一致——输入非空 → 设置新值；
     * 输入为空且勾选"清除" → 置空；否则保留当前值。位置留空 + 勾选清除 =
     * 恢复自动定位（按公网 IP）。API Host 同理（留空 + 勾选清除 = 公共地址）。 */
    bool weather_changed = false;
    const char *new_weather_key = cur->weather_api_key;
    const char *new_weather_loc = cur->weather_location;
    const char *new_weather_host = cur->weather_api_host;
    if (has_weather_key) {
        new_weather_key = (weather_api_key[0] == '\0' && !clear_weather_key)
                              ? cur->weather_api_key
                              : weather_api_key;
    }
    if (has_weather_loc) {
        new_weather_loc = (weather_location[0] == '\0' && !clear_weather_loc)
                              ? cur->weather_location
                              : weather_location;
    }
    if (has_weather_host) {
        new_weather_host = (weather_api_host[0] == '\0' && !clear_weather_host)
                               ? cur->weather_api_host
                               : weather_api_host;
    }
    if (err == ESP_OK && has_weather_key &&
        strcmp(new_weather_key, cur->weather_api_key) != 0) {
        err = espaperplay_system_set_weather_api_key(new_weather_key);
        if (err == ESP_OK) {
            weather_changed = true;
        }
    }
    if (err == ESP_OK && has_weather_loc &&
        strcmp(new_weather_loc, cur->weather_location) != 0) {
        err = espaperplay_system_set_weather_location(new_weather_loc);
        if (err == ESP_OK) {
            weather_changed = true;
        }
    }
    if (err == ESP_OK && has_weather_host &&
        strcmp(new_weather_host, cur->weather_api_host) != 0) {
        err = espaperplay_system_set_weather_api_host(new_weather_host);
        if (err == ESP_OK) {
            weather_changed = true;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
        webserver_send_json_err(req, esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* 先返回成功响应，确保客户端收到反馈，再延时重启 WiFi（仅 WiFi 字段
     * 实际变化时；切换工作模式会重建网络接口并可能改变 IP，重启会断开
     * 当前连接——屏幕设置等非网络字段的保存不会触发）。 */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "wifi_restarted", wifi_changed);
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);

    if (wifi_changed) {
        vTaskDelay(pdMS_TO_TICKS(200));
        err = espaperplay_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to re-apply wifi config: %s", esp_err_to_name(err));
        }
    }
    /* 天气配置变化：清空缓存并请求立即刷新，使新 Key / 位置尽快生效。 */
    if (weather_changed) {
        espaperplay_weather_config_changed();
    }
    return ESP_OK;
}

/** POST /api/config/reset —— 恢复出厂默认并重新应用 WiFi。 */
esp_err_t webserver_handle_config_reset_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t err = espaperplay_system_reset_defaults();
    if (err != ESP_OK) {
        webserver_send_json_err(req, esp_err_to_name(err));
        return ESP_FAIL;
    }
    /* 恢复默认后同步应用到驱动。 */
    espaperplay_epd_set_idle_sleep_timeout_ms(
        espaperplay_system_get_config()->epd_idle_sleep_timeout_ms);

    /* 先返回成功响应，再延时重启 WiFi（恢复默认可能切回 AP 模式并改变 IP）。 */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);

    vTaskDelay(pdMS_TO_TICKS(200));
    err = espaperplay_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to re-apply wifi after reset: %s", esp_err_to_name(err));
    }
    /* 天气配置已恢复默认（Key 清空），同步清空天气缓存。 */
    espaperplay_weather_config_changed();
    return ESP_OK;
}

/** POST /api/wifi/restart —— 重新应用 WiFi 配置。 */
esp_err_t webserver_handle_wifi_restart_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t err = espaperplay_wifi_start();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}
