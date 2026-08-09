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
#include "webserver_internal.h"

static const char *TAG = "ESPaperPlay_WEB_AUTH";

/* POST 表单请求体的最大字节数（超过直接拒绝，防内存放大）。 */
#define ESPAPERPLAY_WEB_FORM_BUF_SIZE 1024

/* ------------------------------------------------------------------ */
/* 鉴权域路由处理器                                                     */
/* ------------------------------------------------------------------ */

/** 读取表单编码的请求体（调用方负责 free）。 */
static esp_err_t read_form_body(httpd_req_t *req, char **out_body) {
    int total = req->content_len;
    if (total <= 0 || total >= ESPAPERPLAY_WEB_FORM_BUF_SIZE) {
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
static esp_err_t issue_session(httpd_req_t *req) {
    char token[ESPAPERPLAY_SESSION_TOKEN_HEX_LEN];
    esp_err_t cerr = espaperplay_session_create(0, token, sizeof(token), NULL);
    if (cerr != ESP_OK) {
        webserver_send_json_err(req, esp_err_to_name(cerr));
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "token", token);
        cJSON_AddBoolToObject(root, "password_configured", espaperplay_auth_is_configured());
        webserver_send_json(req, "200 OK", root);
        cJSON_Delete(root);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
    return ESP_OK;
}

/** POST /api/auth/login —— 密码登录，成功后签发会话 token。 */
esp_err_t webserver_handle_auth_login_post(httpd_req_t *req) {
    /* 出厂未设置密码：免密码登录（首次设置阶段兜底，主要走 /api/auth/password）。 */
    if (!espaperplay_auth_is_configured()) {
        ESP_LOGI(TAG, "Login (passwordless, unconfigured)");
        espaperplay_session_login_success();
        return issue_session(req);
    }

    /* 登录限速：锁定期间直接 429，不执行密码校验（节省 CPU，防暴力破解与 DoS）。 */
    if (!espaperplay_session_login_allowed()) {
        ESP_LOGW(TAG, "Login rejected: in lockout");
        webserver_send_json_err_status(req, "429 Too Many Requests",
                                       "登录尝试过于频繁，请稍后再试");
        return ESP_OK;
    }

    char *body = NULL;
    if (read_form_body(req, &body) != ESP_OK) {
        webserver_send_json_err(req, "请求体过大或为空");
        return ESP_FAIL;
    }
    char password[ESPAPERPLAY_AUTH_PASSWORD_MAX_LEN] = {0};
    bool has_pwd = webserver_form_get_field(body, "password", password, sizeof(password));
    free(body);

    if (!has_pwd || password[0] == '\0') {
        webserver_send_json_err(req, "缺少密码");
        return ESP_FAIL;
    }

    esp_err_t verr = espaperplay_auth_verify(password);
    if (verr != ESP_OK) {
        /* 失败：登记并进入限速流程；区分"密码错误"与"内部错误"。 */
        espaperplay_session_login_failure();
        if (verr == ESP_ERR_NOT_ALLOWED) {
            ESP_LOGW(TAG, "Login failed: wrong password");
            webserver_send_json_err_status(req, "401 Unauthorized", "密码错误");
        } else {
            ESP_LOGW(TAG, "Login failed: %s", esp_err_to_name(verr));
            webserver_send_json_err(req, esp_err_to_name(verr));
        }
        return ESP_OK;
    }

    /* 登录成功：清零失败计数并签发会话。 */
    espaperplay_session_login_success();
    ESP_LOGI(TAG, "Login success");
    return issue_session(req);
}

/** POST /api/auth/password —— 首次设置 / 修改密码。 */
esp_err_t webserver_handle_auth_password_post(httpd_req_t *req) {
    bool configured = espaperplay_auth_is_configured();
    /* 已配置时修改密码需要已登录；首次设置（未配置）免鉴权。 */
    if (configured && webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char *body = NULL;
    if (read_form_body(req, &body) != ESP_OK) {
        webserver_send_json_err(req, "请求体过大或为空");
        return ESP_FAIL;
    }
    char password[ESPAPERPLAY_AUTH_PASSWORD_MAX_LEN] = {0};
    char current[ESPAPERPLAY_AUTH_PASSWORD_MAX_LEN] = {0};
    bool has_pwd = webserver_form_get_field(body, "password", password, sizeof(password));
    bool has_current = webserver_form_get_field(body, "current_password", current, sizeof(current));
    free(body);
    if (!has_pwd || password[0] == '\0') {
        webserver_send_json_err(req, "缺少密码");
        return ESP_FAIL;
    }

    /* 已配置时：修改密码需验证当前密码（防已登录会话被滥用）。 */
    if (configured) {
        if (!has_current || current[0] == '\0') {
            webserver_send_json_err(req, "缺少当前密码");
            return ESP_FAIL;
        }
        if (espaperplay_auth_verify(current) != ESP_OK) {
            /* 当前密码错误：计入登录失败限速，防爆破改密。 */
            espaperplay_session_login_failure();
            webserver_send_json_err(req, "当前密码错误");
            return ESP_OK;
        }
        espaperplay_session_login_success();
    }

    esp_err_t err = espaperplay_auth_change_password(password);
    if (err != ESP_OK) {
        webserver_send_json_err(req, esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* 首次设置（未配置）：清零失败计数并签发会话，前端直接进入管理页。 */
    if (!configured) {
        espaperplay_session_login_success();
        ESP_LOGI(TAG, "Password set (first-time setup)");
        return issue_session(req);
    }

    /* 修改成功：吊销全部会话，强制重新登录。 */
    espaperplay_session_clear_all();
    ESP_LOGI(TAG, "Password changed, all sessions revoked");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "reauthenticate", true);
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** POST /api/auth/logout —— 吊销当前会话。 */
esp_err_t webserver_handle_auth_logout_post(httpd_req_t *req) {
    char token[ESPAPERPLAY_SESSION_TOKEN_HEX_LEN];
    if (webserver_get_bearer_token(req, token, sizeof(token))) {
        espaperplay_session_destroy(token);
        ESP_LOGI(TAG, "Logout");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** GET /api/auth/status —— 登录状态（供前端决定显示登录页还是管理页）。 */
esp_err_t webserver_handle_auth_status_get(httpd_req_t *req) {
    char token[ESPAPERPLAY_SESSION_TOKEN_HEX_LEN];
    bool authed = webserver_get_bearer_token(req, token, sizeof(token)) &&
                  espaperplay_session_verify(token, NULL) == ESP_OK;

    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "authenticated", authed);
        cJSON_AddBoolToObject(root, "password_configured", espaperplay_auth_is_configured());
        cJSON_AddBoolToObject(root, "login_locked", !espaperplay_session_login_allowed());
        cJSON_AddNumberToObject(root, "sessions", (double)espaperplay_session_count());
        webserver_send_json(req, "200 OK", root);
        cJSON_Delete(root);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
    return ESP_OK;
}
