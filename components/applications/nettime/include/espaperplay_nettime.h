/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_nettime.h
 * @brief 网络时间同步应用（串联三个独立功能）。
 *
 * 应用层门面（Facade）：把三个独立服务串成完整链路——
 *
 *   1. netip 服务：通过 UAPI（https://uapis.cn）获取本机公网 IP；
 *   2. geoip 服务：使用相同 IP 查询地理位置（含 IANA 时区）；
 *   3. clock 服务：根据地理位置设置时区，并用 NTP 同步系统时间。
 *
 * 三个服务本身相互独立、可单独调用；本组件仅负责编排与日志输出。
 */

/**
 * @brief 启动网络时间同步流程。
 *
 * 创建一个后台任务：等待 WiFi STA 联网后，依次执行
 * 「获取公网 IP → 查询地理位置 → 设置时区 → NTP 同步」，
 * 并将每一步结果打印到日志。WiFi 未联网（如 AP 模式）或步骤失败时
 * 记录日志后退出，不阻塞系统启动。
 *
 * 应在 WiFi 服务初始化之后调用，可重复调用（幂等）。
 *
 * @return ESP_OK 任务创建成功；否则返回错误码。
 */
esp_err_t espaperplay_nettime_start(void);

#ifdef __cplusplus
}
#endif
