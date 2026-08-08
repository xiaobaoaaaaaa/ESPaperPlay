/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_webserver.h
 * @brief Web 管理服务。
 *
 * 基于 esp_http_server 提供设备管理界面：通过浏览器查看系统运行状态
 * （运行时间 / 剩余堆 / WiFi 状态等），并修改系统设置（WiFi 工作模式与
 * 凭据）。HTTP 服务器监听所有网络接口，AP 与 STA 模式下均可访问。
 *
 * 提供的路由：
 * - GET  /                     设备管理页面（嵌入式 HTML）
 * - GET  /api/status           系统运行状态（JSON）
 * - GET  /api/config           当前系统配置（JSON）
 * - POST /api/config           更新系统配置（表单编码，保存并重新应用 WiFi）
 * - POST /api/config/reset     恢复出厂默认配置并重新应用 WiFi
 * - POST /api/wifi/restart     重新应用 WiFi 配置（模式 / 凭据变更后）
 * - POST /api/system/reboot    重启设备
 *
 * @note 当前版本未实现访问认证，仅适用于局域网 / 可信环境。
 */

/**
 * @brief 启动 Web 服务。
 *
 * 启动 HTTP 服务器并注册全部路由处理器。应在 WiFi 服务初始化之后调用，
 * 以便同时服务于 AP 与 STA 接口。可重复调用，重复调用时先停止旧服务器。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_webserver_start(void);

/**
 * @brief 停止 Web 服务。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_webserver_stop(void);

/**
 * @brief 查询 Web 服务是否正在运行。
 *
 * @return 服务器已启动返回 true。
 */
bool espaperplay_webserver_is_running(void);

#ifdef __cplusplus
}
#endif
