/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espaperplay_config.h"
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
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);

    /* 稍作延时让响应有机会发出，再触发重启。 */
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK; /* 不会执行到这里 */
}
