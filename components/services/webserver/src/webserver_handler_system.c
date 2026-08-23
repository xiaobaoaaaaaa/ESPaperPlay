/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espaperplay_config.h"
#include "espaperplay_storage.h"
#include "espaperplay_wifi.h"
#include "webserver_internal.h"

/* ------------------------------------------------------------------ */
/* 页面 / 系统域路由处理器                                              */
/* ------------------------------------------------------------------ */

/** GET / —— 返回嵌入式管理页面。 */
esp_err_t webserver_handle_root_get(httpd_req_t *req) {
    size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)index_html_start, len);
    return ESP_OK;
}

/** GET /favicon.ico —— 无图标，返回 204 避免 404 日志刷屏。 */
esp_err_t webserver_handle_favicon_get(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/** GET /api/status —— 系统运行状态。 */
esp_err_t webserver_handle_status_get(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    cJSON *fw = cJSON_AddObjectToObject(root, "firmware");
    cJSON_AddStringToObject(fw, "name", ESPAPERPLAY_PROJECT_NAME);
    cJSON_AddStringToObject(fw, "version", ESPAPERPLAY_VERSION);
    cJSON_AddStringToObject(fw, "idf_version", esp_get_idf_version());

    cJSON *sys = cJSON_AddObjectToObject(root, "system");
    cJSON_AddNumberToObject(sys, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(sys, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "min_free_heap", (double)esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(sys, "chip", CONFIG_IDF_TARGET);
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddNumberToObject(sys, "cores", chip_info.cores);
    cJSON_AddNumberToObject(sys, "revision", chip_info.revision);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    espaperplay_wifi_status_t wifi_status;
    if (espaperplay_wifi_get_status(&wifi_status) == ESP_OK) {
        cJSON_AddBoolToObject(wifi, "started", wifi_status.started);
        cJSON_AddBoolToObject(wifi, "connected", wifi_status.connected);
        cJSON_AddStringToObject(wifi, "mode",
                                wifi_status.mode == ESPAPERPLAY_WIFI_MODE_AP ? "AP" : "STA");
        cJSON_AddStringToObject(wifi, "ssid", wifi_status.ssid);
        cJSON_AddStringToObject(wifi, "ip", wifi_status.ip);
    }

    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** POST /api/system/reboot —— 重启设备。 */
esp_err_t webserver_handle_reboot_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);

    /* 稍作延时让响应有机会发出；随后安全下电 SD 卡（刷盘 + 卸载 + CMD0
     * 软下电）再重启，避免重启瞬间卡片上有未收尾的事务。失败不阻断重启。 */
    vTaskDelay(pdMS_TO_TICKS(200));
    if (espaperplay_storage_is_mounted()) {
        esp_err_t um_ret = espaperplay_storage_unmount();
        if (um_ret != ESP_OK) {
            ESP_LOGW("ESPaperPlay_WEBSRV", "SD unmount before reboot failed: %s",
                     esp_err_to_name(um_ret));
        }
    }
    esp_restart();
    return ESP_OK; /* 不会执行到这里 */
}

/** GET/POST * (HTTP:80) —— 302 重定向到 HTTPS，杜绝明文访问。 */
esp_err_t webserver_handle_redirect_to_https(httpd_req_t *req) {
    char host[128] = {0};
    /* URI 最长 CONFIG_HTTPD_MAX_URI_LEN(512)，加 host 与协议前缀后仍不截断。 */
    char location[768];

    /* 优先取 Host 头（浏览器访问 http://<host>/... 时携带）。 */
    size_t hlen = httpd_req_get_hdr_value_len(req, "Host");
    if (hlen > 0 && hlen < sizeof(host)) {
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    }

    /* 兜底：Host 缺失时取 WiFi 当前 IP（AP 或 STA）。 */
    if (host[0] == '\0') {
        espaperplay_wifi_status_t st;
        if (espaperplay_wifi_get_status(&st) == ESP_OK && st.ip[0] != '\0' &&
            strcmp(st.ip, "0.0.0.0") != 0) {
            snprintf(host, sizeof(host), "%s", st.ip);
        }
    }

    /* 去掉 Host 中附带的端口段（如 ":80"）。局域网为 IPv4，简单处理最后一个冒号。 */
    char *colon = strrchr(host, ':');
    if (colon != NULL && colon != host) {
        bool digits_only = true;
        for (const char *p = colon + 1; *p != '\0'; p++) {
            if (*p < '0' || *p > '9') {
                digits_only = false;
                break;
            }
        }
        if (digits_only) {
            *colon = '\0';
        }
    }

    if (host[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown host");
        return ESP_FAIL;
    }

    snprintf(location, sizeof(location), "https://%s%s", host, req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
