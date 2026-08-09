/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "espaperplay_webserver.h"
#include "webserver_internal.h"

static const char *TAG = "ESPaperPlay_WEB";

/*!< HTTP 服务器句柄，NULL 表示未启动 */
static httpd_handle_t s_server = NULL;

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
    config.max_uri_handlers = 16;
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
