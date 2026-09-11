/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log_level.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_diaglog.h
 * @brief SD 卡日志：全局日志捕获（等级可配）+ 关键诊断事件，逐行落盘，
 *        供长时间无人值守监测与事后回看（触摸失效等偶发问题的现场留存）。
 *
 * 两个写入通道，落在同一组文件中：
 *   1. 全局捕获：接管 esp_log 输出（esp_log_set_vprintf），按配置的最低
 *      等级过滤（默认 Warning 及以上，Web 管理页 / 设备设置页可调）后经
 *      RAM 队列异步落盘；串口输出原样保留，不受影响；
 *   2. 显式事件：espaperplay_diaglog_write() 直写的策划性诊断事件
 *      （开机 / 睡眠周期 / 触摸现场），不受等级过滤，同步落盘。
 *
 * 文件组织（ESPAPERPLAY_SYSTEM_SD_DIR 下，Web 文件管理器可直接下载）：
 *   - diag-YYYYMMDD.log：按天分文件，超 ESPAPERPLAY_DIAGLOG_MAX_BYTES
 *     轮转为同名 .old；
 *   - diag-boot.log：墙钟未同步（NTP 前）期间的行，同步后自动切换按天文件；
 *   - 定期清理：落盘任务在开机与日期切换时删除超过
 *     ESPAPERPLAY_DIAGLOG_RETENTION_DAYS 天的旧日志。
 *
 * 特性：
 *   - 捕获通道在专用落盘任务（大栈）做 stdio/FATFS 写链路，日志调用方只
 *     付出一次 vsnprintf + 入队成本，不受小任务栈限制；
 *   - 逐行「打开-追加-关闭」，掉电/异常重启最多丢当前一行，不依赖常驻句柄；
 *   - 锁竞争超时或 SD 不可用时静默丢弃，绝不阻塞调用方；
 *   - SD 就绪前的行在 PSRAM 环形缓冲保留最近若干条，首次写成功后自动补写；
 *   - 每行自带墙钟时间（NTP 同步后）与开机秒数，睡眠周期也能对齐时间线。
 */

/** 单文件上限：超过后轮转为 .old（单日总占用上限 = 2 倍该值）。 */
#define ESPAPERPLAY_DIAGLOG_MAX_BYTES (1024 * 1024)

/** 旧日志保留天数（按文件名日期判定；boot 文件按 mtime 判定）。 */
#define ESPAPERPLAY_DIAGLOG_RETENTION_DAYS 7

/** 墙钟未同步期间的事件日志文件名（位于 ESPAPERPLAY_SYSTEM_SD_DIR 下）。 */
#define ESPAPERPLAY_DIAGLOG_BOOT_NAME "diag-boot.log"

/**
 * @brief 初始化 SD 日志（创建锁 / 捕获队列 / 落盘任务，并接管 esp_log 输出）。
 *
 * 应在 NVS/系统配置加载后调用（等级默认 Warning；如需按配置调整，
 * 初始化后再调 espaperplay_diaglog_set_level()）。SD 卡稍后才挂载亦可：
 * 未就绪期间的行进入环形缓冲，挂载并首次写成功后自动补写。
 *
 * @return ESP_OK；锁 / 任务 / 缓冲创建失败返回 ESP_ERR_NO_MEM（此时仅
 *         显式事件通道可用，全局捕获停用）。
 */
esp_err_t espaperplay_diaglog_init(void);

/**
 * @brief 追加一行显式诊断事件（不受等级过滤，同步落盘）。
 *
 * @param tag 事件来源标签（如 "TOUCH" / "PWR" / "BOOT"），置于行内。
 * @param fmt printf 风格格式串（单行，勿带换行）。
 *
 * @return 已写入返回 true；未初始化 / SD 不可用 / 锁竞争超时返回 false。
 */
bool espaperplay_diaglog_write(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * @brief 设置全局捕获的最低日志等级（立即生效，不持久化）。
 *
 * 只影响捕获通道；显式事件始终落盘。可捕获的最高等级受固件编译选项
 * CONFIG_LOG_MAXIMUM_LEVEL 限制（当前固件为 Info，Debug/Verbose 在编译期
 * 已被裁剪，即使设置也不会有输出）。
 *
 * @param level 最低等级（ESP_LOG_NONE = 关闭捕获）。
 */
void espaperplay_diaglog_set_level(esp_log_level_t level);

/**
 * @brief 获取当前全局捕获的最低日志等级。
 *
 * @return 当前等级。
 */
esp_log_level_t espaperplay_diaglog_get_level(void);

#ifdef __cplusplus
}
#endif
