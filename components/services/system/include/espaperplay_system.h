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

/** 和风天气 API Key 最大长度（含结尾 '\0'）。 */
#define ESPAPERPLAY_SYSTEM_WEATHER_KEY_MAX_LEN 48
/** 和风天气位置（LocationID / 城市名 / "经度,纬度"）最大长度（含结尾 '\0'）。 */
#define ESPAPERPLAY_SYSTEM_WEATHER_LOC_MAX_LEN 64
/** 和风天气自定义 API Host 最大长度（含结尾 '\0'，如 "abc1234xyz.def.qweatherapi.com"）。 */
#define ESPAPERPLAY_SYSTEM_WEATHER_HOST_MAX_LEN 80

/** 出厂默认 WiFi 工作模式。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_WIFI_MODE ESPAPERPLAY_WIFI_MODE_AP
/** 出厂默认 STA 模式 SSID。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_STA_SSID "ESPaperPlay"
/** 出厂默认 STA 模式密码（留空表示开放网络）。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_STA_PASS ""
/** 出厂默认 AP 模式 SSID。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_AP_SSID "ESPaperPlay-AP"
/** 出厂默认 AP 模式密码（留空表示开放网络）。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_AP_PASS ""

/** 出厂默认屏幕空闲自动睡眠超时（毫秒；与 ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS 一致，0=关闭）。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_EPD_IDLE_SLEEP_TIMEOUT_MS 30000

/** 出厂默认"连续大面积局刷 N 次后强制全刷"阈值（0=禁用，只局刷）。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_GUI_FULL_FORCE_AFTER 5

/** 出厂默认和风天气 API Key（空 = 未配置，天气服务不工作）。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_API_KEY ""
/** 出厂默认和风天气位置（空 = 按公网 IP 自动定位）。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_LOCATION ""
/** 出厂默认和风天气自定义 API Host（空 = 使用公共地址 devapi/geoapi.qweather.com）。 */
#define ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_API_HOST ""

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
    uint32_t epd_idle_sleep_timeout_ms;                 /*!< 屏幕空闲自动睡眠超时（毫秒，0=关闭） */
    uint32_t gui_full_force_after; /*!< 连续大面积局刷后强制全刷阈值（0=禁用） */
    char weather_api_key[ESPAPERPLAY_SYSTEM_WEATHER_KEY_MAX_LEN]; /*!< 和风天气 API Key（空=未配置） */
    char weather_location[ESPAPERPLAY_SYSTEM_WEATHER_LOC_MAX_LEN]; /*!< 和风天气位置（空=自动定位） */
    char weather_api_host[ESPAPERPLAY_SYSTEM_WEATHER_HOST_MAX_LEN]; /*!< 和风天气自定义 API Host（空=公共地址） */
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
 * @brief 设置屏幕空闲自动睡眠超时（毫秒，0=关闭）并持久化。
 *
 * @param timeout_ms 超时毫秒数（0 表示关闭自动睡眠）。
 * @return 成功返回 ESP_OK；超过 24 小时（86400000）返回 ESP_ERR_INVALID_ARG；
 *         NVS 写入失败返回错误码。
 */
esp_err_t espaperplay_system_set_epd_idle_sleep_timeout_ms(uint32_t timeout_ms);

/**
 * @brief 设置"连续大面积局刷 N 次后强制全刷"阈值（0=禁用）并持久化。
 *
 * @param count 阈值（0..255；0 表示永不强制全刷，只做局部刷新）。
 * @return 成功返回 ESP_OK；越界返回 ESP_ERR_INVALID_ARG；NVS 写入失败返回错误码。
 */
esp_err_t espaperplay_system_set_gui_full_force_after(uint32_t count);

/**
 * @brief 设置和风天气 API Key 并持久化。
 *
 * @param key API Key（长度 < ESPAPERPLAY_SYSTEM_WEATHER_KEY_MAX_LEN，可为空串表示清除）。
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE。
 */
esp_err_t espaperplay_system_set_weather_api_key(const char *key);

/**
 * @brief 设置和风天气位置并持久化。
 *
 * 支持 LocationID（如 "101010100"）、城市名（如 "北京"）或 "经度,纬度"
 * （如 "116.41,39.92"，兼容 "纬度,经度" 自动交换）；传空串表示清除手动
 * 位置，天气服务将按公网 IP 自动定位。
 *
 * @param location 位置（长度 < ESPAPERPLAY_SYSTEM_WEATHER_LOC_MAX_LEN，可为空串）。
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE。
 */
esp_err_t espaperplay_system_set_weather_location(const char *location);

/**
 * @brief 设置和风天气自定义 API Host 并持久化。
 *
 * 和风天气自 2026 年起逐步停止公共 API 地址（devapi/geoapi.qweather.com），
 * 建议在控制台-设置中查看并填入自己的 API Host（如
 * "abc1234xyz.def.qweatherapi.com"）。传空串表示清除自定义 Host，
 * 回退到默认公共地址。
 *
 * @param host API Host（长度 < ESPAPERPLAY_SYSTEM_WEATHER_HOST_MAX_LEN，可为空串）。
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE。
 */
esp_err_t espaperplay_system_set_weather_api_host(const char *host);

/**
 * @brief 恢复出厂默认配置并持久化。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_system_reset_defaults(void);

#ifdef __cplusplus
}
#endif
