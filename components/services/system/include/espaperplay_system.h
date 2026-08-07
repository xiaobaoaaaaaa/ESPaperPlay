/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_system.h
 * @brief 系统配置服务。
 *
 * 将系统级配置持久化到 NVS，覆盖 WiFi 工作模式（AP / STA）以及两种模式下的
 * SSID 与密码。系统启动时由 espaperplay_system_init() 从 NVS 加载；
 * 运行期可通过访问器读取，修改后立即写回 NVS（掉电不丢失）。
 */

/** NVS 配置命名空间。 */
#define ESPAPERPLAY_SYSTEM_NVS_NAMESPACE "system"

/** SSID 最大长度（含结尾 '\0'）。 */
#define ESPAPERPLAY_SYSTEM_SSID_MAX_LEN 32
/** WiFi 密码最大长度（含结尾 '\0'）。 */
#define ESPAPERPLAY_SYSTEM_PASS_MAX_LEN 64

/** 出厂默认 WiFi 工作模式。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_WIFI_MODE ESPAPERPLAY_WIFI_MODE_AP
/** 出厂默认 STA 模式 SSID。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_STA_SSID "ESPaperPlay"
/** 出厂默认 STA 模式密码（留空表示开放网络）。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_STA_PASS ""
/** 出厂默认 AP 模式 SSID。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_AP_SSID "ESPaperPlay-AP"
/** 出厂默认 AP 模式密码。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_AP_PASS "espapaper"

/**
 * @brief WiFi 工作模式。
 */
typedef enum {
    ESPAPERPLAY_WIFI_MODE_AP = 0, /*!< 热点模式 */
    ESPAPERPLAY_WIFI_MODE_STA,    /*!< 站点模式 */
    ESPAPERPLAY_WIFI_MODE_MAX,
} espaperplay_wifi_mode_t;

/**
 * @brief 系统配置快照。
 *
 * 字符串字段均以 '\0' 结尾，容量由 ESPAPERPLAY_SYSTEM_*_MAX_LEN 定义。
 */
typedef struct {
    espaperplay_wifi_mode_t wifi_mode;                  /*!< WiFi 工作模式 */
    char sta_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN];     /*!< STA 模式 SSID */
    char sta_password[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN]; /*!< STA 模式密码 */
    char ap_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN];      /*!< AP 模式 SSID */
    char ap_password[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN];  /*!< AP 模式密码 */
} espaperplay_system_config_t;

/**
 * @brief 初始化系统配置服务。
 *
 * 挂载 NVS 并从其中加载全部配置；对缺失或非法的字段回填出厂默认值并写回。
 * 应在其他依赖配置的模块（如 WiFi）之前调用，且仅需调用一次。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_system_init(void);

/**
 * @brief 获取当前系统配置。
 *
 * 返回指向内部配置副本的只读指针，其生命周期与系统相同，无需释放。
 *
 * @warning 返回的指针在后续任何 setter 调用后内容可能变化，请勿跨调用长期持有。
 *
 * @return 系统配置指针（恒非空）。
 */
const espaperplay_system_config_t *espaperplay_system_get_config(void);

/**
 * @brief 设置 WiFi 工作模式并持久化。
 *
 * @param mode 目标工作模式（AP 或 STA）。
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；NVS 写入失败返回错误码。
 */
esp_err_t espaperplay_system_set_wifi_mode(espaperplay_wifi_mode_t mode);

/**
 * @brief 设置 STA 模式凭据（SSID 与密码）并持久化。
 *
 * @param ssid STA 模式 SSID（长度 < ESPAPERPLAY_SYSTEM_SSID_MAX_LEN）。
 * @param password STA 模式密码（长度 < ESPAPERPLAY_SYSTEM_PASS_MAX_LEN，可为空串）。
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE。
 */
esp_err_t espaperplay_system_set_sta_credentials(const char *ssid, const char *password);

/**
 * @brief 设置 AP 模式凭据（SSID 与密码）并持久化。
 *
 * @param ssid AP 模式 SSID（长度 < ESPAPERPLAY_SYSTEM_SSID_MAX_LEN）。
 * @param password AP 模式密码（长度 < ESPAPERPLAY_SYSTEM_PASS_MAX_LEN，可为空串）。
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE。
 */
esp_err_t espaperplay_system_set_ap_credentials(const char *ssid, const char *password);

/**
 * @brief 恢复出厂默认配置并持久化。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_system_reset_defaults(void);

#ifdef __cplusplus
}
#endif
