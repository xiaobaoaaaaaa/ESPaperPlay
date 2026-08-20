/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "esp_http_server.h"

#include "espaperplay_auth.h"
#include "espaperplay_session.h"
#include "webserver_internal.h"

/* Authorization 头格式：Bearer <token>。 */
#define AUTH_HEADER_BUF_SIZE 80
#define BEARER_PREFIX "Bearer "
#define BEARER_PREFIX_LEN (sizeof(BEARER_PREFIX) - 1)

/* ------------------------------------------------------------------ */
/* 鉴权守卫                                                             */
/* ------------------------------------------------------------------ */

bool webserver_get_bearer_token(httpd_req_t *req, char *token, size_t token_size) {
    char buf[AUTH_HEADER_BUF_SIZE];
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    if (strncmp(buf, BEARER_PREFIX, BEARER_PREFIX_LEN) != 0) {
        return false;
    }
    const char *tok = buf + BEARER_PREFIX_LEN;
    size_t tok_len = strlen(tok);
    if (tok_len == 0 || tok_len >= token_size) {
        return false;
    }
    strlcpy(token, tok, token_size);
    return true;
}

/**
 * @brief 受保护接口的鉴权守卫。
 *
 * 校验 Authorization: Bearer <token> 是否有效；失败时发送 401 并返回
 * ESP_FAIL，调用方应直接 return。出厂未设置密码时免鉴权放行（首次设置）。
 */
esp_err_t webserver_require_auth(httpd_req_t *req) {
    /* 出厂未设置密码：处于首次设置阶段，免鉴权放行。 */
    if (!espaperplay_auth_is_configured()) {
        return ESP_OK;
    }

    char token[ESPAPERPLAY_SESSION_TOKEN_HEX_LEN];
    if (!webserver_get_bearer_token(req, token, sizeof(token))) {
        webserver_send_json_err_status(req, "401 Unauthorized", "未登录");
        return ESP_FAIL;
    }
    if (espaperplay_session_verify(token, NULL) != ESP_OK) {
        webserver_send_json_err_status(req, "401 Unauthorized", "会话无效或已过期");
        return ESP_FAIL;
    }
    return ESP_OK;
}
