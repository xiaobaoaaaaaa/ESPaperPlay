/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
 * @brief 以指定状态码 + JSON 错误对象响应。
 *
 * @param req    当前请求。
 * @param status 响应状态行（如 "401 Unauthorized"）。
 * @param msg    错误描述。
 */
void webserver_send_json_err_status(httpd_req_t *req, const char *status, const char *msg);

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

/**
 * @brief 读取表单编码的请求体（小表单；NUL 结尾，调用方负责 free）。
 *
 * 失败（过大/为空/读取中断/无内存）时内部已响应对应错误，返回 NULL，
 * 调用方直接返回 ESP_FAIL 即可。
 */
char *webserver_read_form_body(httpd_req_t *req);

/**
 * @brief 以 {"ok":true} 响应（200）。
 */
void webserver_send_ok(httpd_req_t *req);

/**
 * @brief 把请求体（total 字节）流式分块写入已打开的文件。
 *
 * 任何失败都会 fclose 并 remove(rollback_path) 清理半成品，同时内部已响应
 * 错误。成功时返回写入字节数（=total），失败返回 -1（文件已被关闭）。
 */
int webserver_recv_body_to_file(httpd_req_t *req, int total, FILE *f, const char *rollback_path);

/**
 * @brief 从 URL 查询串提取字段（等价于 query_str + form_get_field 组合）。
 */
bool webserver_query_get_field(httpd_req_t *req, const char *name, char *out, size_t out_size);

/**
 * @brief 延时后重启设备（调用前先完成响应发送）。
 */
void webserver_restart_after_response(void);

/**
 * @brief 签发会话并以 JSON 响应（token + password_configured）。
 *
 * 登录成功与首次引导设置完成后共用（webserver_auth_guard.c）。
 * @return ESP_OK=已响应；ESP_FAIL=签发失败（已响应错误）。
 */
esp_err_t webserver_issue_session_json(httpd_req_t *req);

/* ------------------------------------------------------------------ */
/* 路由处理器                                                           */
/* ------------------------------------------------------------------ */

/* 页面 / 系统域（webserver_handler_system.c）。 */
esp_err_t webserver_handle_root_get(httpd_req_t *req);
esp_err_t webserver_handle_favicon_get(httpd_req_t *req);
esp_err_t webserver_handle_status_get(httpd_req_t *req);
esp_err_t webserver_handle_reboot_post(httpd_req_t *req);
esp_err_t webserver_handle_heartbeat_post(httpd_req_t *req);
esp_err_t webserver_handle_touch_diag_post(httpd_req_t *req);

/* 配置域（webserver_handler_config.c）。 */
esp_err_t webserver_handle_config_get(httpd_req_t *req);
esp_err_t webserver_handle_config_post(httpd_req_t *req);
esp_err_t webserver_handle_config_reset_post(httpd_req_t *req);
esp_err_t webserver_handle_wifi_restart_post(httpd_req_t *req);
esp_err_t webserver_handle_wifi_scan_get(httpd_req_t *req);

/* 引导域（webserver_handler_setup.c）：首次开机分步向导（公开免鉴权，
 * 仅引导未完成时开放写接口）。 */
esp_err_t webserver_handle_setup_status_get(httpd_req_t *req);
esp_err_t webserver_handle_setup_apply_post(httpd_req_t *req);

/* 天气域（webserver_handler_weather.c）。 */
esp_err_t webserver_handle_weather_get(httpd_req_t *req);
esp_err_t webserver_handle_weather_refresh_post(httpd_req_t *req);

/* 字体域（webserver_handler_fonts.c）：SD 卡字体枚举 / 选择 / 上传 / 删除。 */
esp_err_t webserver_handle_fonts_get(httpd_req_t *req);
esp_err_t webserver_handle_fonts_select_post(httpd_req_t *req);
esp_err_t webserver_handle_fonts_upload_post(httpd_req_t *req);
esp_err_t webserver_handle_fonts_delete_post(httpd_req_t *req);

/* 文件域（webserver_handler_files.c）：SD 卡文件浏览 / 上传 / 下载 /
 * 新建文件夹 / 重命名 / 删除。 */
esp_err_t webserver_handle_files_get(httpd_req_t *req);
esp_err_t webserver_handle_files_download_get(httpd_req_t *req);
esp_err_t webserver_handle_files_upload_post(httpd_req_t *req);
esp_err_t webserver_handle_files_mkdir_post(httpd_req_t *req);
esp_err_t webserver_handle_files_rename_post(httpd_req_t *req);
esp_err_t webserver_handle_files_delete_post(httpd_req_t *req);

/* 鉴权守卫（webserver_auth_guard.c）。 */
esp_err_t webserver_require_auth(httpd_req_t *req);
bool webserver_get_bearer_token(httpd_req_t *req, char *token, size_t token_size);

/* 鉴权域（webserver_handler_auth.c）。 */
esp_err_t webserver_handle_auth_login_post(httpd_req_t *req);
esp_err_t webserver_handle_auth_password_post(httpd_req_t *req);
esp_err_t webserver_handle_auth_logout_post(httpd_req_t *req);
esp_err_t webserver_handle_auth_status_get(httpd_req_t *req);

/* TLS 证书服务（webserver_tls.c）：运行时生成/加载自签名证书与私钥。 */
esp_err_t webserver_tls_get(const uint8_t **cert, size_t *cert_len, const uint8_t **key,
                            size_t *key_len);

/* TLS 证书刷新（webserver_tls.c）：IP 变化时重新生成证书，out_changed 报告是否更新。 */
esp_err_t webserver_tls_refresh(bool *out_changed);

/* HTTP 重定向（webserver_handler_system.c）：把明文请求 302 到 HTTPS。 */
esp_err_t webserver_handle_redirect_to_https(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
