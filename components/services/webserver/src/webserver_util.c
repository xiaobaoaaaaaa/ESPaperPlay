/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_system.h"

#include "webserver_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

void webserver_send_ok(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* 表单请求体 / 流式上传 / 重启                                          */
/* ------------------------------------------------------------------ */

/** 表单请求体最大字节数（超过直接拒绝，防内存放大）。 */
#define WEBSERVER_FORM_BUF_MAX 1024
/** 流式上传的读写分块大小。 */
#define WEBSERVER_IO_CHUNK 4096

char *webserver_read_form_body(httpd_req_t *req) {
    const int total = req->content_len;
    if (total <= 0 || total >= WEBSERVER_FORM_BUF_MAX) {
        webserver_send_json_err(req, "请求体过大或为空");
        return NULL;
    }
    char *body = malloc((size_t)total + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return NULL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, (size_t)(total - received));
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "读取请求体失败");
            return NULL;
        }
        received += r;
    }
    body[received] = '\0';
    return body;
}

int webserver_recv_body_to_file(httpd_req_t *req, int total, FILE *f, const char *rollback_path) {
    /* 分块读取请求体写入 SD，避免大文件整体驻留 RAM。 */
    char buf[WEBSERVER_IO_CHUNK];
    int received = 0;
    while (received < total) {
        int chunk = total - received;
        if (chunk > (int)sizeof(buf)) {
            chunk = (int)sizeof(buf);
        }
        int r = httpd_req_recv(req, buf, (size_t)chunk);
        if (r <= 0) {
            fclose(f);
            remove(rollback_path); /* 半途而废的残留一并清理 */
            webserver_send_json_err(req, "读取请求体失败");
            return -1;
        }
        received += r;
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
            fclose(f);
            remove(rollback_path);
            webserver_send_json_err(req, "写入 SD 卡失败");
            return -1;
        }
    }
    fclose(f);
    return received;
}

bool webserver_query_get_field(httpd_req_t *req, const char *name, char *out, size_t out_size) {
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len <= 1) {
        return false;
    }
    char *query = malloc(buf_len);
    if (query == NULL || httpd_req_get_url_query_str(req, query, buf_len) != ESP_OK) {
        free(query);
        return false;
    }
    const bool found = webserver_form_get_field(query, name, out, out_size);
    free(query);
    return found;
}

void webserver_restart_after_response(void) {
    /* 稍作延时让响应有机会发出，再触发重启。 */
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
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
