/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_diaglog.h
 * @brief SD 卡诊断日志：把关键诊断事件逐行追加到 SD 卡文件，供长时间
 *        无人值守监测与事后回看（触摸失效等偶发问题的现场留存）。
 *
 * 特性：
 *   - 逐行「打开-追加-关闭」，掉电/异常重启最多丢当前一行，不依赖常驻句柄；
 *   - 互斥串行化，任意任务上下文可调用；锁竞争超时或 SD 不可用时静默丢弃
 *     （串口日志不受影响），绝不阻塞调用方；
 *   - 文件超限自动轮转（diag.log -> diag.log.old），占用上限固定；
 *   - 每行自带墙钟时间（NTP 同步后）与开机秒数，睡眠周期也能对齐时间线。
 *
 * 日志文件位于 ESPAPERPLAY_SYSTEM_SD_DIR "/diag.log"，可通过 Web 文件
 * 管理器下载，或取下 SD 卡直接读取。
 */

/** 诊断日志文件绝对路径（VFS）。 */
#define ESPAPERPLAY_DIAGLOG_PATH ESPAPERPLAY_SYSTEM_SD_DIR "/diag.log"

/**
 * @brief 初始化诊断日志（创建互斥锁与目标目录）。
 *
 * 应在 SD 卡挂载后调用；未初始化或 SD 稍后才可用时，写入会静默丢弃/
 * 自动恢复，不要求严格时序。
 *
 * @return ESP_OK；锁创建失败返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_diaglog_init(void);

/**
 * @brief 追加一行诊断日志。
 *
 * @param tag 事件来源标签（如 "TOUCH" / "PWR" / "BOOT"），置于行内。
 * @param fmt printf 风格格式串（单行，勿带换行）。
 *
 * @return 已写入返回 true；未初始化 / SD 不可用 / 锁竞争超时返回 false。
 */
bool espaperplay_diaglog_write(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif
