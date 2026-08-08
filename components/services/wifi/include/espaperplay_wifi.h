/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "espaperplay_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_wifi.h
 * @brief WiFi 网络服务。
 *
 * 根据系统配置（espaperplay_system.h）中保存的工作模式（AP / STA）与
 * SSID / 密码，启动并维护 ESP-IDF 的 WiFi 连接：
 *
 * - AP 模式：以配置的 SSID / 密码开启热点，密码为空时按开放网络（WIFI_AUTH_OPEN）启动；
 * - STA 模式：连接配置的目标 AP，密码为空时按开放网络连接；
 * - STA 断线后自动重连（有限次数），避免长时间离线；
 * - 系统启动阶段（仅首次启动）若 STA 用尽重连次数仍无法连接，自动回退到 AP 模式，
 *   确保设备开箱即可被连接发现；运行期重连失败则仅放弃，不会改变工作模式。
 *
 * 该组件依赖系统配置服务，因此 espaperplay_wifi_init() 必须在
 * espaperplay_system_init() 之后调用。
 */

/** STA 断线后的自动重连最大次数（超过后放弃，直到下次重新启动）。 */
#define ESPAPERPLAY_WIFI_STA_RECONNECT_RETRY_MAX 10
/** 启动阶段 STA 连接失败后回退到 AP 模式前的延时（毫秒）。 */
#define ESPAPERPLAY_WIFI_FALLBACK_DELAY_MS 1000
/** AP 热点允许的最大客户端连接数。 */
#define ESPAPERPLAY_WIFI_AP_MAX_CONNECTION 4
/** AP 热点信道（0 表示自动选择）。 */
#define ESPAPERPLAY_WIFI_AP_CHANNEL 0

/**
 * @brief WiFi 运行状态快照。
 */
typedef struct {
    bool started;                 /*!< WiFi 驱动是否已启动 */
    bool connected;               /*!< 网络是否可用：AP 模式指热点已开启，STA 模式指已获取 IP */
    espaperplay_wifi_mode_t mode; /*!< 当前工作模式 */
    char ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN]; /*!< 当前使用的 SSID */
    char ip[16]; /*!< 当前 IPv4 地址字符串（如 "192.168.4.1"），未分配为 "0.0.0.0" */
} espaperplay_wifi_status_t;

/**
 * @brief 初始化 WiFi 网络服务。
 *
 * 完成底层框架初始化（netif / 事件循环 / 事件处理器），并根据系统配置
 * 立即启动 WiFi。本函数只应调用一次，重复调用会返回 ESP_OK 但不再动作。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_wifi_init(void);

/**
 * @brief 启动 / 重启 WiFi。
 *
 * 读取最新系统配置，按其中的工作模式（AP / STA）与凭据配置并启动 WiFi。
 * 可用于配置变更后重新应用（如切换模式或修改 SSID 之后）。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_wifi_start(void);

/**
 * @brief 停止 WiFi。
 *
 * 关闭 WiFi 驱动并清除连接状态；之后可通过 espaperplay_wifi_start() 重新启动。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_wifi_stop(void);

/**
 * @brief 获取当前 WiFi 运行状态。
 *
 * @param status 输出状态快照（非空）。
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_wifi_get_status(espaperplay_wifi_status_t *status);

/**
 * @brief 查询网络是否可用。
 *
 * @return AP 模式且热点已开启、或 STA 模式且已获取 IP 时返回 true。
 */
bool espaperplay_wifi_is_connected(void);

#ifdef __cplusplus
}
#endif
