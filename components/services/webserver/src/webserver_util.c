/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"

#include "webserver_internal.h"

/* ------------------------------------------------------------------ */
/* 通用 HTTP / JSON 辅助                                                */
/* ------------------------------------------------------------------ */

void webserver_send_json(httpd_req_t *req, const char *status, const cJSON *root) {
    char *body = cJSON_PrintUnformatted(root);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON serialization failed");
        return;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, strlen(body));
    free(body);
}

void webserver_send_json_err_status(httpd_req_t *req, const char *status, const char *msg) {
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddStringToObject(root, "error", msg);
        webserver_send_json(req, status, root);
        cJSON_Delete(root);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
}

void webserver_send_json_err(httpd_req_t *req, const char *msg) {
    webserver_send_json_err_status(req, "400 Bad Request", msg);
}

/* ------------------------------------------------------------------ */
/* 表单解析                                                             */
/* ------------------------------------------------------------------ */

/** 十六进制字符转数值，非法字符返回 -1。 */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * @brief 解码 URL 编码字符串（application/x-www-form-urlencoded）。
 *
 * '%XX' 还原为字节，'+' 还原为空格；解码后以 '\0' 结尾写入 dst。
 * 输出容量由 dst_size 限制，超长部分截断。
 */
static void url_decode(const char *src, char *dst, size_t dst_size) {
    size_t i = 0, j = 0;
    while (src[i] != '\0' && j + 1 < dst_size) {
        int hi = hex_val(src[i + 1]);
        int lo = hex_val(src[i + 2]);
        if (src[i] == '%' && hi >= 0 && lo >= 0) {
            dst[j++] = (char)(hi * 16 + lo);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

bool webserver_form_get_field(const char *body, const char *name, char *out, size_t out_size) {
    size_t name_len = strlen(name);
    const char *p = body;

    while (*p != '\0') {
        const char *end = strchr(p, '&');
        if (end == NULL) {
            end = p + strlen(p);
        }
        /* 匹配 name= 前缀（'&' 分隔的独立字段）。 */
        if ((size_t)(end - p) > name_len && strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            const char *val = p + name_len + 1;
            size_t val_len = (size_t)(end - val);
            char *tmp = malloc(val_len + 1);
            if (tmp == NULL) {
                return false;
            }
            memcpy(tmp, val, val_len);
            tmp[val_len] = '\0';

            char decoded[3 * out_size + 1];
            url_decode(tmp, decoded, sizeof(decoded));
            strlcpy(out, decoded, out_size);
            free(tmp);
            return true;
        }
        p = (*end == '&') ? end + 1 : end;
    }
    return false;
}

bool webserver_form_get_flag(const char *body, const char *name) {
    char buf[8] = {0};
    if (!webserver_form_get_field(body, name, buf, sizeof(buf))) {
        return false;
    }
    return strcmp(buf, "1") == 0 || strcmp(buf, "true") == 0 || strcmp(buf, "on") == 0;
}
