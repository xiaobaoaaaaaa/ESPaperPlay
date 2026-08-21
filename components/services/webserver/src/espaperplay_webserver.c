/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espaperplay_webserver.h"
#include "webserver_internal.h"

static const char *TAG = "ESPaperPlay_WEB";

/*!< HTTPS(443) 主服务器句柄，NULL 表示未启动 */
static httpd_handle_t s_server_https = NULL;

/*!< HTTP(80) 重定向服务器句柄，仅把所有请求 302 到 HTTPS */
static httpd_handle_t s_server_http = NULL;

/*!< TLS 刷新任务防重入标志（IP 事件可能连续触发）。 */
static volatile bool s_tls_refresh_busy = false;

/** TLS 刷新任务栈大小（证书生成 / 服务器重启均为重活）。 */
#define TLS_REFRESH_TASK_STACK 8192
#define TLS_REFRESH_TASK_PRIO 5

/**
 * @brief 注册全部路由处理器。
 *
 * 处理器按业务域分散在 webserver_handler_*.c 中，此处仅维护路由表；
 * 新增接口时在此追加一行即可。
 */
static esp_err_t register_handlers(httpd_handle_t server) {
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = webserver_handle_root_get, .user_ctx = NULL},
        {.uri = "/favicon.ico",
         .method = HTTP_GET,
         .handler = webserver_handle_favicon_get,
         .user_ctx = NULL},
        {.uri = "/api/status",
         .method = HTTP_GET,
         .handler = webserver_handle_status_get,
         .user_ctx = NULL},
        {.uri = "/api/config",
         .method = HTTP_GET,
         .handler = webserver_handle_config_get,
         .user_ctx = NULL},
        {.uri = "/api/config",
         .method = HTTP_POST,
         .handler = webserver_handle_config_post,
         .user_ctx = NULL},
        {.uri = "/api/config/reset",
         .method = HTTP_POST,
         .handler = webserver_handle_config_reset_post,
         .user_ctx = NULL},
        {.uri = "/api/wifi/restart",
         .method = HTTP_POST,
         .handler = webserver_handle_wifi_restart_post,
         .user_ctx = NULL},
        {.uri = "/api/system/reboot",
         .method = HTTP_POST,
         .handler = webserver_handle_reboot_post,
         .user_ctx = NULL},
        {.uri = "/api/auth/status",
         .method = HTTP_GET,
         .handler = webserver_handle_auth_status_get,
         .user_ctx = NULL},
        {.uri = "/api/auth/login",
         .method = HTTP_POST,
         .handler = webserver_handle_auth_login_post,
         .user_ctx = NULL},
        {.uri = "/api/auth/password",
         .method = HTTP_POST,
         .handler = webserver_handle_auth_password_post,
         .user_ctx = NULL},
        {.uri = "/api/auth/logout",
         .method = HTTP_POST,
         .handler = webserver_handle_auth_logout_post,
         .user_ctx = NULL},
        {.uri = "/api/weather",
         .method = HTTP_GET,
         .handler = webserver_handle_weather_get,
         .user_ctx = NULL},
        {.uri = "/api/weather/refresh",
         .method = HTTP_POST,
         .handler = webserver_handle_weather_refresh_post,
         .user_ctx = NULL},
        {.uri = "/api/fonts",
         .method = HTTP_GET,
         .handler = webserver_handle_fonts_get,
         .user_ctx = NULL},
        {.uri = "/api/fonts/select",
         .method = HTTP_POST,
         .handler = webserver_handle_fonts_select_post,
         .user_ctx = NULL},
        {.uri = "/api/fonts/upload",
         .method = HTTP_POST,
         .handler = webserver_handle_fonts_upload_post,
         .user_ctx = NULL},
        {.uri = "/api/fonts/delete",
         .method = HTTP_POST,
         .handler = webserver_handle_fonts_delete_post,
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

/**
 * @brief 启动 HTTPS 主服务器并注册全部路由。
 *
 * 供 espaperplay_webserver_start() 与 IP 变化刷新复用。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
static esp_err_t start_https_server(void) {
    /* 获取（或首次生成）运行时自签名证书与私钥。 */
    const uint8_t *cert = NULL, *key = NULL;
    size_t cert_len = 0, key_len = 0;
    esp_err_t err = webserver_tls_get(&cert, &cert_len, &key, &key_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to obtain TLS certificate: %s", esp_err_to_name(err));
        return err;
    }

    /* HTTPS 主服务器（443）：承载全部业务路由。 */
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.stack_size = 10240; /* TLS 握手需要较大栈 */
    conf.httpd.max_uri_handlers = 24;
    conf.servercert = cert;
    conf.servercert_len = cert_len;
    conf.prvtkey_pem = key;
    conf.prvtkey_len = key_len;

    err = httpd_ssl_start(&s_server_https, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTPS server: %s", esp_err_to_name(err));
        return err;
    }
    err = register_handlers(s_server_https);
    if (err != ESP_OK) {
        httpd_ssl_stop(s_server_https);
        s_server_https = NULL;
        return err;
    }
    return ESP_OK;
}

/**
 * @brief 一次性任务：刷新证书（SAN 含新 IP）并在必要时重启 HTTPS。
 *
 * 事件处理器（sys_evt 小栈任务）只负责创建本任务，重活在此独立大栈
 * 任务中执行，避免证书生成 / 服务器重启导致 sys_evt 栈溢出。
 */
static void webserver_tls_refresh_task(void *arg) {
    (void)arg;

    bool changed = false;
    esp_err_t err = webserver_tls_refresh(&changed);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to refresh TLS cert on IP change");
    } else if (changed && s_server_https != NULL) {
        ESP_LOGI(TAG, "TLS cert updated, restarting HTTPS server");
        httpd_ssl_stop(s_server_https);
        s_server_https = NULL;
        if (start_https_server() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restart HTTPS server after IP change");
        }
    }

    s_tls_refresh_busy = false;
    vTaskDelete(NULL);
}

/**
 * @brief STA 获取新 IP 的事件处理器。
 *
 * 仅检查是否需要刷新并创建独立任务执行，避免在 sys_evt 事件任务中
 * 做证书生成等 CPU / 栈密集操作。
 */
static void webserver_on_sta_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    if (s_server_https == NULL || s_tls_refresh_busy) {
        return;
    }
    s_tls_refresh_busy = true;
    if (xTaskCreate(webserver_tls_refresh_task, "tls_refresh", TLS_REFRESH_TASK_STACK, NULL,
                    TLS_REFRESH_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create TLS refresh task");
        s_tls_refresh_busy = false;
    }
}

esp_err_t espaperplay_webserver_start(void) {
    if (s_server_https != NULL) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }

    /* 自签名证书场景下，任何未信任证书的客户端连接都会在握手时被拒绝，
     * esp-tls / esp_https_server / httpd 会刷出大量 -0x7780(FATAL_ALERT)
     * 与 accept(EAGAIN) 错误日志，属无害噪音；调低日志级别避免刷屏。
     * 排查 TLS 问题时可将这些 TAG 级别调回 ESP_LOG_INFO。 */
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_NONE);
    esp_log_level_set("esp_https_server", ESP_LOG_NONE);
    esp_log_level_set("httpd", ESP_LOG_NONE);
    esp_log_level_set("httpd_uri", ESP_LOG_NONE);

    esp_err_t err = start_https_server();
    if (err != ESP_OK) {
        return err;
    }

    /* HTTP 重定向服务器（80）：所有请求 302 到 HTTPS，杜绝明文流量。
     * 注意：HTTPD_DEFAULT_CONFIG 的 uri_match_fn 为 NULL（精确匹配），
     * 通配符路由（斜杠+星号）需显式启用 wildcard 匹配器。 */
    httpd_config_t http_conf = HTTPD_DEFAULT_CONFIG();
    http_conf.max_uri_handlers = 2;
    http_conf.uri_match_fn = httpd_uri_match_wildcard;
    static const httpd_uri_t redirect_uris[] = {
        {.uri = "/*", .method = HTTP_GET, .handler = webserver_handle_redirect_to_https},
        {.uri = "/*", .method = HTTP_POST, .handler = webserver_handle_redirect_to_https},
    };
    err = httpd_start(&s_server_http, &http_conf);
    if (err != ESP_OK) {
        /* 80 端口不可用仅损失便利性，HTTPS 主服务不受影响。 */
        ESP_LOGW(TAG, "Failed to start HTTP redirect server: %s", esp_err_to_name(err));
        s_server_http = NULL;
    } else {
        for (size_t i = 0; i < sizeof(redirect_uris) / sizeof(redirect_uris[0]); i++) {
            if (httpd_register_uri_handler(s_server_http, &redirect_uris[i]) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to register redirect handler");
            }
        }
    }

    /* STA 获取新 IP 时刷新证书（SAN 含新 IP）并重启 HTTPS。 */
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, webserver_on_sta_got_ip, NULL);

    ESP_LOGI(TAG, "Web server started: HTTPS on :443, HTTP on :80 (redirect to HTTPS)");
    return ESP_OK;
}

esp_err_t espaperplay_webserver_stop(void) {
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, webserver_on_sta_got_ip);
    if (s_server_http != NULL) {
        httpd_stop(s_server_http);
        s_server_http = NULL;
    }
    if (s_server_https != NULL) {
        httpd_ssl_stop(s_server_https);
        s_server_https = NULL;
    }
    return ESP_OK;
}

bool espaperplay_webserver_is_running(void) { return s_server_https != NULL; }
