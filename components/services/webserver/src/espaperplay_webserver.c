/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espaperplay_config.h"
#include "espaperplay_system.h"
#include "espaperplay_webserver.h"
#include "espaperplay_wifi.h"

static const char *TAG = "ESPaperPlay_WEB";

/* ------------------------------------------------------------------ */
/* 嵌入式 Web 页面                                                      */
/* ------------------------------------------------------------------ */

/* 由 CMake 的 EMBED_TXTFILES 嵌入的 www/index.html（符号名取文件名去特殊字符）。 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* POST 表单请求体的最大字节数（超过直接拒绝，防内存放大）。 */
#define ESPAPERPLAY_WEB_FORM_BUF_SIZE 1024

/* ------------------------------------------------------------------ */
/* 服务器状态                                                           */
/* ------------------------------------------------------------------ */

static httpd_handle_t s_server = NULL; /*!< HTTP 服务器句柄，NULL 表示未启动 */

/* ------------------------------------------------------------------ */
/* 通用响应辅助                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief 以 JSON 响应，自动设置 Content-Type、Cache-Control 并序列化。
 */
static void send_json(httpd_req_t *req, const char *status, const cJSON *root) {
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

/** 以 400 + JSON 错误对象响应。 */
static void send_json_err(httpd_req_t *req, const char *msg) {
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddStringToObject(root, "error", msg);
        send_json(req, "400 Bad Request", root);
        cJSON_Delete(root);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
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

/**
 * @brief 从表单编码的请求体中提取字段值。
 *
 * 取第一个名为 name 的 'name=value' 项，URL 解码后写入 out。
 *
 * @return 找到字段返回 true，否则返回 false（out 保持不变）。
 */
static bool form_get_field(const char *body, const char *name, char *out, size_t out_size) {
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

/** 读取布尔开关字段（值为 1 / true / on 时视为真）。 */
static bool form_get_flag(const char *body, const char *name) {
    char buf[8] = {0};
    if (!form_get_field(body, name, buf, sizeof(buf))) {
        return false;
    }
    return strcmp(buf, "1") == 0 || strcmp(buf, "true") == 0 || strcmp(buf, "on") == 0;
}

/* ------------------------------------------------------------------ */
/* 路由处理器                                                           */
/* ------------------------------------------------------------------ */

/** GET / —— 返回嵌入式管理页面。 */
static esp_err_t handle_root_get(httpd_req_t *req) {
    size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)index_html_start, len);
    return ESP_OK;
}

/** GET /favicon.ico —— 无图标，返回 204 避免 404 日志刷屏。 */
static esp_err_t handle_favicon_get(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/** GET /api/status —— 系统运行状态。 */
static esp_err_t handle_status_get(httpd_req_t *req) {
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

    send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** GET /api/config —— 返回当前系统配置。 */
static esp_err_t handle_config_get(httpd_req_t *req) {
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

    send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** POST /api/config —— 更新系统配置并重新应用 WiFi。 */
static esp_err_t handle_config_post(httpd_req_t *req) {
    /* 读取表单编码的请求体。 */
    int total = req->content_len;
    if (total <= 0 || total >= ESPAPERPLAY_WEB_FORM_BUF_SIZE) {
        send_json_err(req, "请求体过大或为空");
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

    /* 解析字段。 */
    char wifi_mode_str[8] = {0};
    char sta_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN] = {0};
    char sta_password[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN] = {0};
    char ap_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN] = {0};
    char ap_password[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN] = {0};
    form_get_field(body, "wifi_mode", wifi_mode_str, sizeof(wifi_mode_str));
    form_get_field(body, "sta_ssid", sta_ssid, sizeof(sta_ssid));
    form_get_field(body, "sta_password", sta_password, sizeof(sta_password));
    form_get_field(body, "ap_ssid", ap_ssid, sizeof(ap_ssid));
    form_get_field(body, "ap_password", ap_password, sizeof(ap_password));
    bool clear_sta_password = form_get_flag(body, "clear_sta_password");
    bool clear_ap_password = form_get_flag(body, "clear_ap_password");
    free(body);

    /* 解析工作模式。 */
    espaperplay_wifi_mode_t mode;
    if (strcmp(wifi_mode_str, "sta") == 0) {
        mode = ESPAPERPLAY_WIFI_MODE_STA;
    } else if (strcmp(wifi_mode_str, "ap") == 0) {
        mode = ESPAPERPLAY_WIFI_MODE_AP;
    } else {
        send_json_err(req, "无效的 wifi_mode");
        return ESP_FAIL;
    }

    const espaperplay_system_config_t *cur = espaperplay_system_get_config();

    /* 密码语义：输入非空 → 设置新值；输入为空且勾选"清除" → 置空；否则保留当前值。 */
    const char *new_sta_pass =
        (sta_password[0] == '\0' && !clear_sta_password) ? cur->sta_password : sta_password;
    const char *new_ap_pass =
        (ap_password[0] == '\0' && !clear_ap_password) ? cur->ap_password : ap_password;

    /* 校验：所选模式对应的 SSID 不能为空。 */
    if (mode == ESPAPERPLAY_WIFI_MODE_STA && sta_ssid[0] == '\0') {
        send_json_err(req, "STA SSID 不能为空");
        return ESP_FAIL;
    }
    if (mode == ESPAPERPLAY_WIFI_MODE_AP && ap_ssid[0] == '\0') {
        send_json_err(req, "AP SSID 不能为空");
        return ESP_FAIL;
    }

    /* 依次应用并持久化到 NVS。 */
    esp_err_t err = ESP_OK;
    if (mode != cur->wifi_mode) {
        err = espaperplay_system_set_wifi_mode(mode);
    }
    if (err == ESP_OK &&
        (strcmp(sta_ssid, cur->sta_ssid) != 0 || strcmp(new_sta_pass, cur->sta_password) != 0)) {
        err = espaperplay_system_set_sta_credentials(sta_ssid, new_sta_pass);
    }
    if (err == ESP_OK &&
        (strcmp(ap_ssid, cur->ap_ssid) != 0 || strcmp(new_ap_pass, cur->ap_password) != 0)) {
        err = espaperplay_system_set_ap_credentials(ap_ssid, new_ap_pass);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
        send_json_err(req, esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* 重新应用 WiFi 配置（模式 / 凭据可能变化，可能短暂断开当前连接，属预期行为）。 */
    err = espaperplay_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to re-apply wifi config: %s", esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "wifi_restarted", err == ESP_OK);
    send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** POST /api/config/reset —— 恢复出厂默认并重新应用 WiFi。 */
static esp_err_t handle_config_reset_post(httpd_req_t *req) {
    esp_err_t err = espaperplay_system_reset_defaults();
    if (err == ESP_OK) {
        err = espaperplay_wifi_start();
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** POST /api/wifi/restart —— 重新应用 WiFi 配置。 */
static esp_err_t handle_wifi_restart_post(httpd_req_t *req) {
    esp_err_t err = espaperplay_wifi_start();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** POST /api/system/reboot —— 重启设备。 */
static esp_err_t handle_reboot_post(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    send_json(req, "200 OK", root);
    cJSON_Delete(root);

    /* 稍作延时让响应有机会发出，再触发重启。 */
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK; /* 不会执行到这里 */
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                            */
/* ------------------------------------------------------------------ */

/** 注册全部路由处理器。 */
static esp_err_t register_handlers(httpd_handle_t server) {
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_root_get, .user_ctx = NULL},
        {.uri = "/favicon.ico",
         .method = HTTP_GET,
         .handler = handle_favicon_get,
         .user_ctx = NULL},
        {.uri = "/api/status", .method = HTTP_GET, .handler = handle_status_get, .user_ctx = NULL},
        {.uri = "/api/config", .method = HTTP_GET, .handler = handle_config_get, .user_ctx = NULL},
        {.uri = "/api/config",
         .method = HTTP_POST,
         .handler = handle_config_post,
         .user_ctx = NULL},
        {.uri = "/api/config/reset",
         .method = HTTP_POST,
         .handler = handle_config_reset_post,
         .user_ctx = NULL},
        {.uri = "/api/wifi/restart",
         .method = HTTP_POST,
         .handler = handle_wifi_restart_post,
         .user_ctx = NULL},
        {.uri = "/api/system/reboot",
         .method = HTTP_POST,
         .handler = handle_reboot_post,
         .user_ctx = NULL},
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register uri '%s': %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t espaperplay_webserver_start(void) {
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; /* JSON 序列化 / 表单解析需要较大栈 */
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
        return err;
    }

    err = register_handlers(s_server);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Web server started on port %u", config.server_port);
    return ESP_OK;
}

esp_err_t espaperplay_webserver_stop(void) {
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

bool espaperplay_webserver_is_running(void) { return s_server != NULL; }
