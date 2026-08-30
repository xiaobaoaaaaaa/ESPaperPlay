/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "espaperplay_auth.h"
#include "espaperplay_session.h"
#include "espaperplay_system.h"
#include "espaperplay_wifi.h"
#include "webserver_internal.h"

static const char *TAG = "ESPaperPlay_WEB_SETUP";

/* POST 表单请求体的最大字节数（超过直接拒绝，防内存放大）。 */
#define ESPAPERPLAY_WEB_SETUP_FORM_BUF_SIZE 1024

/* ------------------------------------------------------------------ */
/* 公开性守卫                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief 引导期公开写接口守卫。
 *
 * 首次开机引导期间（setup_done=false）这些接口免鉴权开放，便于用户在
 * 未设密码、未登录状态下完成初始化；一旦引导完成（setup_done=true），
 * 所有写接口必须登录后经 /api/config 等常规接口操作，此处统一拒绝。
 *
 * @return true=允许（引导未完成）；false=拒绝（应返回 403）。
 */
static bool setup_write_allowed(void) { return !espaperplay_system_is_setup_done(); }

/** 读取表单编码的请求体（调用方负责 free）。 */
static esp_err_t setup_read_form_body(httpd_req_t *req, char **out_body) {
    int total = req->content_len;
    if (total <= 0 || total >= ESPAPERPLAY_WEB_SETUP_FORM_BUF_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *body = malloc((size_t)total + 1);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, (size_t)(total - received));
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';
    *out_body = body;
    return ESP_OK;
}

/** 签发会话并返回 JSON（token + password_configured）。 */
static esp_err_t setup_issue_session(httpd_req_t *req) {
    char token[ESPAPERPLAY_SESSION_TOKEN_HEX_LEN];
    esp_err_t cerr = espaperplay_session_create(0, token, sizeof(token), NULL);
    if (cerr != ESP_OK) {
        webserver_send_json_err(req, esp_err_to_name(cerr));
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "token", token);
    cJSON_AddBoolToObject(root, "password_configured", espaperplay_auth_is_configured());
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 路由处理器                                                           */
/* ------------------------------------------------------------------ */

/** GET /api/setup/status —— 引导状态（公开，无需鉴权）。 */
esp_err_t webserver_handle_setup_status_get(httpd_req_t *req) {
    espaperplay_wifi_status_t st;
    const bool wifi_ok = espaperplay_wifi_get_status(&st) == ESP_OK;
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "setup_done", espaperplay_system_is_setup_done());
    cJSON_AddBoolToObject(root, "password_configured", espaperplay_auth_is_configured());
    cJSON_AddStringToObject(root, "wifi_mode",
                            cfg->wifi_mode == ESPAPERPLAY_WIFI_MODE_AP ? "ap" : "sta");
    cJSON_AddStringToObject(root, "ap_ssid", cfg->ap_ssid);
    /* STA SSID 仅回传是否已配置（布尔），避免公开接口泄露网络名。 */
    cJSON_AddBoolToObject(root, "sta_ssid_set", cfg->sta_ssid[0] != '\0');
    cJSON_AddBoolToObject(root, "weather_key_set", cfg->weather_api_key[0] != '\0');
    if (wifi_ok) {
        cJSON_AddStringToObject(root, "ip", st.ip);
        cJSON_AddBoolToObject(root, "ap_open", st.mode == ESPAPERPLAY_WIFI_MODE_AP && st.connected);
    }
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** POST /api/setup/apply —— 引导期一次性写入全部配置（仅引导未完成时开放）。
 *
 * 收集密码 / 网络 / 天气后于最后一步统一落盘，避免中途重启导致部分写入
 * （如仅写入密码即重启，重启后向导重现却因 password_configured 而卡住）。
 */
esp_err_t webserver_handle_setup_apply_post(httpd_req_t *req) {
    if (!setup_write_allowed()) {
        webserver_send_json_err_status(req, "403 Forbidden", "引导已完成，请登录后修改配置");
        return ESP_OK;
    }

    char *body = NULL;
    if (setup_read_form_body(req, &body) != ESP_OK) {
        webserver_send_json_err(req, "请求体过大或为空");
        return ESP_FAIL;
    }

    /* 密码（可选，首次设置）。 */
    char password[ESPAPERPLAY_AUTH_PASSWORD_MAX_LEN] = {0};
    const bool has_pwd = webserver_form_get_field(body, "password", password, sizeof(password));

    /* 网络。 */
    char wifi_mode_str[8] = {0};
    char sta_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN] = {0};
    char sta_password[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN] = {0};
    char ap_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN] = {0};
    char ap_password[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN] = {0};
    const bool has_mode =
        webserver_form_get_field(body, "wifi_mode", wifi_mode_str, sizeof(wifi_mode_str));
    const bool has_sta_ssid =
        webserver_form_get_field(body, "sta_ssid", sta_ssid, sizeof(sta_ssid));
    const bool has_sta_pass =
        webserver_form_get_field(body, "sta_password", sta_password, sizeof(sta_password));
    const bool has_ap_ssid = webserver_form_get_field(body, "ap_ssid", ap_ssid, sizeof(ap_ssid));
    const bool has_ap_pass =
        webserver_form_get_field(body, "ap_password", ap_password, sizeof(ap_password));

    /* 天气（可选）。 */
    char weather_api_key[ESPAPERPLAY_SYSTEM_WEATHER_KEY_MAX_LEN] = {0};
    char weather_location[ESPAPERPLAY_SYSTEM_WEATHER_LOC_MAX_LEN] = {0};
    char weather_api_host[ESPAPERPLAY_SYSTEM_WEATHER_HOST_MAX_LEN] = {0};
    const bool has_key =
        webserver_form_get_field(body, "weather_api_key", weather_api_key, sizeof(weather_api_key));
    const bool has_loc = webserver_form_get_field(body, "weather_location", weather_location,
                                                  sizeof(weather_location));
    const bool has_host = webserver_form_get_field(body, "weather_api_host", weather_api_host,
                                                   sizeof(weather_api_host));

    free(body);

    const espaperplay_system_config_t *cur = espaperplay_system_get_config();
    esp_err_t err = ESP_OK;

    /* 解析模式（缺席 = 保持当前）。 */
    espaperplay_wifi_mode_t mode = cur->wifi_mode;
    if (has_mode) {
        if (strcmp(wifi_mode_str, "sta") == 0) {
            mode = ESPAPERPLAY_WIFI_MODE_STA;
        } else if (strcmp(wifi_mode_str, "ap") == 0) {
            mode = ESPAPERPLAY_WIFI_MODE_AP;
        } else {
            webserver_send_json_err(req, "无效的 wifi_mode");
            return ESP_FAIL;
        }
    }

    /* 校验：出现且为目标模式时 SSID 不能为空。 */
    if (has_sta_ssid && mode == ESPAPERPLAY_WIFI_MODE_STA && sta_ssid[0] == '\0') {
        webserver_send_json_err(req, "STA SSID 不能为空");
        return ESP_FAIL;
    }
    if (has_ap_ssid && mode == ESPAPERPLAY_WIFI_MODE_AP && ap_ssid[0] == '\0') {
        webserver_send_json_err(req, "AP SSID 不能为空");
        return ESP_FAIL;
    }
    if (has_host && weather_api_host[0] == '\0') {
        webserver_send_json_err(req, "API Host 不能为空");
        return ESP_FAIL;
    }
    if (has_key && weather_api_key[0] == '\0') {
        webserver_send_json_err(req, "API Key 不能为空");
        return ESP_FAIL;
    }

    /* 1) 密码（首次）。 */
    bool pwd_set = false;
    if (has_pwd && password[0] != '\0') {
        if (espaperplay_auth_is_configured()) {
            webserver_send_json_err(req, "密码已配置，请登录后修改");
            return ESP_OK;
        }
        err = espaperplay_auth_change_password(password);
        if (err != ESP_OK) {
            webserver_send_json_err(req, esp_err_to_name(err));
            return ESP_FAIL;
        }
        espaperplay_session_login_success();
        pwd_set = true;
    }

    /* 2) 网络。 */
    if (err == ESP_OK && has_mode && mode != cur->wifi_mode) {
        err = espaperplay_system_set_wifi_mode(mode);
    }
    if (err == ESP_OK && (has_sta_ssid || has_sta_pass)) {
        const char *new_pass = has_sta_pass ? sta_password : cur->sta_password;
        err = espaperplay_system_set_sta_credentials(has_sta_ssid ? sta_ssid : cur->sta_ssid,
                                                     new_pass);
    }
    if (err == ESP_OK && (has_ap_ssid || has_ap_pass)) {
        const char *new_pass = has_ap_pass ? ap_password : cur->ap_password;
        err = espaperplay_system_set_ap_credentials(has_ap_ssid ? ap_ssid : cur->ap_ssid, new_pass);
    }

    /* 3) 天气（可选）。 */
    if (err == ESP_OK && has_key && strcmp(weather_api_key, cur->weather_api_key) != 0) {
        err = espaperplay_system_set_weather_api_key(weather_api_key);
    }
    if (err == ESP_OK && has_loc && strcmp(weather_location, cur->weather_location) != 0) {
        err = espaperplay_system_set_weather_location(weather_location);
    }
    if (err == ESP_OK && has_host && strcmp(weather_api_host, cur->weather_api_host) != 0) {
        err = espaperplay_system_set_weather_api_host(weather_api_host);
    }

    /* 4) 标记引导完成（最后执行，确保前述写入均成功）。 */
    if (err == ESP_OK) {
        err = espaperplay_system_mark_setup_done();
    }

    if (err != ESP_OK) {
        webserver_send_json_err(req, esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* 响应：设置了密码则签发会话令牌，否则仅返回 ok。 */
    if (pwd_set) {
        setup_issue_session(req);
    } else {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        webserver_send_json(req, "200 OK", root);
        cJSON_Delete(root);
    }

    /* 先响应再应用网络（切换模式会重建接口并可能改变 IP）。 */
    vTaskDelay(pdMS_TO_TICKS(200));
    if (espaperplay_wifi_start() != ESP_OK) {
        ESP_LOGW(TAG, "setup: failed to re-apply wifi");
    }
    return ESP_OK;
}
