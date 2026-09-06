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
 * @brief 等待 NTP 时间同步完成。
 *
 * 等待组件自持的同步事件（任一次成功对时置位，取走即清除）。多任务并发
 * 等待安全：同一次同步会唤醒所有正在等待的任务（广播语义）。SNTP 已同步过
 * 且事件未被消费时立即返回成功。
 *
 * @param timeout_ms 等待超时（毫秒），0 表示不等待立即返回。
 * @return ESP_OK 已同步；ESP_ERR_TIMEOUT 超时未同步；其他为错误码。
 */
esp_err_t espaperplay_clock_ntp_wait_sync(uint32_t timeout_ms);

/**
 * @brief 获取按当前时区转换的本地时间。
 *
 * 若已测得 RTC 漂移率，则按软件漂移补偿模型校正后输出（补偿 INT_RC
 * 慢时钟误差，无需外部 32k 晶振）；否则直接返回系统时钟读数。
 *
 * @param local_time 输出本地时间（非空）。
 * @return ESP_OK 成功（时间可能尚未经 NTP 同步，由调用方判断）。
 */
esp_err_t espaperplay_clock_get_local_time(struct tm *local_time);

/**
 * @brief 强制立即通过 NTP 重新对时。
 *
 * 重启 SNTP 轮询（立即发起新一轮同步采样）后阻塞等待一次同步（用于用户
 * 唤醒 / 周期标定等需要即时校正的场景）。不销毁 SNTP 客户端单例——nettime
 * 任务可能正并发等待对时。同步成功后自动更新内部校正基准。
 *
 * @param timeout_ms 等待同步超时（毫秒）。
 * @return ESP_OK 已同步；ESP_ERR_TIMEOUT 超时；其他为错误码。
 */
esp_err_t espaperplay_clock_resync_now(uint32_t timeout_ms);

/**
 * @brief 记录一次成功的时间同步（更新校正基准）。
 *
 * 在任何 NTP 成功对时后调用，使软件漂移补偿的参考点保持最新。通常由本组件
 * 内部在对时完成时自动调用，也可由外部（如 nettime 初始化对时）显式调用。
 *
 * @return ESP_OK 成功。
 */
esp_err_t espaperplay_clock_mark_synced(void);

/**
 * @brief 执行一次时钟标定：NTP 对时并测量/精修 RTC 漂移率，持久化到 NVS。
 *
 * 漂移率通过本次对时与上次测量基线的偏差推算。学习期（NVS 无有效值）以
 * 较短间隔多采样本，稳定期以稀疏间隔定期精修。
 *
 * @param timeout_ms NTP 同步超时（毫秒）。
 * @return ESP_OK 标定成功；否则返回错误码（网络不可用等）。
 */
esp_err_t espaperplay_clock_calibrate(uint32_t timeout_ms);

/**
 * @brief 判断当前是否到达时钟标定时刻。
 *
 * 学习期返回较短间隔、稳定期返回较长间隔；从未标定过则立即返回 true。
 *
 * @return true 需要标定，false 尚未到期。
 */
bool espaperplay_clock_is_calibration_due(void);

/**
 * @brief 获取当前测得的 RTC 漂移率（ppm，+表示偏快）。
 *
 * @return 漂移率（ppm）；尚未测得时返回 0。
 */
int32_t espaperplay_clock_get_drift_ppm(void);

/**
 * @brief 累计一次浅睡眠的测得时长（微秒），供漂移模型仅对睡眠部分补偿。
 *
 * 由电源管理在每次唤醒后调用，传入本次睡眠期间 RC 测得的时长
 * （esp_timer 在睡眠前后差值，即 time(NULL) 在睡眠中推进的量）。
 * 运行期时间由 XTAL 精确推进、不引入误差，故仅睡眠时长需计入。
 *
 * @param sleep_us 本次睡眠的 RC 测得时长（微秒）。
 */
void espaperplay_clock_account_sleep(uint64_t sleep_us);

#ifdef __cplusplus
}
#endif
