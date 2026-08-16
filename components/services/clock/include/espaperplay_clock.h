/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_clock.h
 * @brief 系统时钟服务（功能 3：根据地理位置设置时区，使用 NTP 同步时间）。
 *
 * 提供三块能力：
 *  - 时区管理：设置 IANA 时区（如 "Asia/Shanghai"）并持久化到 NVS，
 *    重启后自动恢复（espaperplay_clock_init() 时应用）。ESP-IDF 的 libc
 *    tzset() 只支持 POSIX 格式的 TZ 字符串，本服务内部会把 IANA 时区名
 *    翻译为 POSIX 格式（内置常用时区映射表，未收录的时区保持当前值并
 *    告警）再应用；
 *  - NTP 同步：通过 esp_netif_sntp 启动 SNTP 客户端（默认多个 NTP 服务器），
 *    并可等待首次时间同步完成；
 *  - 本地时间：同步后按当前时区输出本地时间。
 *
 * 时区名称通常来自地理位置查询（geoip 服务返回的 time_zone 字段，
 * 如 "Asia/Shanghai"），但本组件独立可复用：调用方只需传入时区名称，
 * 不依赖任何其他服务。
 */

/** 时区名称最大长度（含结尾 NUL）。 */
#define ESPAPERPLAY_CLOCK_TZ_MAX_LEN 64
/** 默认时区（未持久化任何时区时使用）。 */
#define ESPAPERPLAY_CLOCK_DEFAULT_TZ "UTC"
/** 默认 NTP 服务器数量。 */
#define ESPAPERPLAY_CLOCK_NTP_SERVER_COUNT 3

/**
 * @brief 初始化时钟服务。
 *
 * 从 NVS 恢复上次设置的时区（不存在时使用默认时区）并应用到系统
 * （setenv("TZ") + tzset()）。应在设置时区 / 启动 NTP 之前调用一次。
 *
 * @return ESP_OK 成功；NVS 读取失败返回错误码（不阻断应用，时区回退默认值）。
 */
esp_err_t espaperplay_clock_init(void);

/**
 * @brief 设置系统时区。
 *
 * 应用 IANA 时区名称（如 "Asia/Shanghai"、"Europe/Berlin"），并持久化到
 * NVS，重启后由 espaperplay_clock_init() 自动恢复。若 tz_name 为 NULL 或
 * 空字符串，则恢复为默认时区（UTC）。
 *
 * @param tz_name IANA 时区名称。
 * @return ESP_OK 成功；参数过长 / NVS 写入失败返回错误码。
 */
esp_err_t espaperplay_clock_set_timezone(const char *tz_name);

/**
 * @brief 获取当前生效的时区名称。
 *
 * @param tz_out     输出缓冲区（非空）。
 * @param tz_out_len 输出缓冲区大小。
 * @return ESP_OK 成功。
 */
esp_err_t espaperplay_clock_get_timezone(char *tz_out, size_t tz_out_len);

/**
 * @brief 启动 NTP 时间同步。
 *
 * 初始化并启动 SNTP 客户端（esp_netif_sntp），使用默认 NTP 服务器列表。
 * 重复调用安全：已启动时直接返回 ESP_OK。
 *
 * 注意：依赖 CONFIG_LWIP_SNTP_MAX_SERVERS >= ESPAPERPLAY_CLOCK_NTP_SERVER_COUNT
 * （项目 sdkconfig.defaults 已配置）。
 *
 * @return ESP_OK 成功，否则返回错误码。
 */
esp_err_t espaperplay_clock_ntp_start(void);

/**
 * @brief 等待 NTP 首次时间同步完成。
 *
 * @param timeout_ms 等待超时（毫秒），0 表示不等待立即返回。
 * @return ESP_OK 已同步；ESP_ERR_TIMEOUT 超时未同步；其他为错误码。
 */
esp_err_t espaperplay_clock_ntp_wait_sync(uint32_t timeout_ms);

/**
 * @brief 获取按当前时区转换的本地时间。
 *
 * @param local_time 输出本地时间（非空）。
 * @return ESP_OK 成功（时间可能尚未经 NTP 同步，由调用方判断）。
 */
esp_err_t espaperplay_clock_get_local_time(struct tm *local_time);

#ifdef __cplusplus
}
#endif
