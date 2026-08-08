/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file webserver_internal.h
 * @brief Web 服务内部共享接口。
 *
 * 仅供 webserver 组件内部各源文件使用，不对外暴露，内容包含：
 * - 嵌入式页面符号（由 CMake EMBED_TXTFILES 生成）；
 * - 通用 HTTP/JSON 响应与表单解析辅助（webserver_util.c）；
 * - 各业务域的路由处理器声明。
 *
 * 新增一个业务域（如存储管理、OTA 升级）时：新建 webserver_handler_*.c，
 * 在此声明其处理器，并在 espaperplay_webserver.c 的路由表中注册即可，
 * 无需改动其他文件。
 */

/* 嵌入式 Web 页面（www/index.html）的起始 / 结束符号。 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* ------------------------------------------------------------------ */
/* 通用 HTTP / JSON 辅助（webserver_util.c）                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 以 JSON 响应，自动设置 Content-Type、Cache-Control 并序列化。
 *
 * @param req    当前请求。
 * @param status 响应状态行（如 "200 OK"）。
 * @param root   待序列化的 JSON 根对象（调用方负责释放）。
 */
void webserver_send_json(httpd_req_t *req, const char *status, const cJSON *root);

/**
 * @brief 以 400 + JSON 错误对象响应。
 *
 * @param req 当前请求。
 * @param msg 错误描述。
 */
void webserver_send_json_err(httpd_req_t *req, const char *msg);

/**
 * @brief 从表单编码的请求体中提取字段值。
 *
 * 取第一个名为 name 的 'name=value' 项，URL 解码后写入 out。
 *
 * @return 找到字段返回 true，否则返回 false（out 保持不变）。
 */
bool webserver_form_get_field(const char *body, const char *name, char *out, size_t out_size);

/**
 * @brief 读取布尔开关字段（值为 1 / true / on 时视为真）。
 */
bool webserver_form_get_flag(const char *body, const char *name);

/* ------------------------------------------------------------------ */
/* 路由处理器                                                           */
/* ------------------------------------------------------------------ */

/* 页面 / 系统域（webserver_handler_system.c）。 */
esp_err_t webserver_handle_root_get(httpd_req_t *req);
esp_err_t webserver_handle_favicon_get(httpd_req_t *req);
esp_err_t webserver_handle_status_get(httpd_req_t *req);
esp_err_t webserver_handle_reboot_post(httpd_req_t *req);

/* 配置域（webserver_handler_config.c）。 */
esp_err_t webserver_handle_config_get(httpd_req_t *req);
esp_err_t webserver_handle_config_post(httpd_req_t *req);
esp_err_t webserver_handle_config_reset_post(httpd_req_t *req);
esp_err_t webserver_handle_wifi_restart_post(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
