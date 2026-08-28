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
 * @file espaperplay_nvs.h
 * @brief 应用层 NVS 统一管理（命名空间登记 + 恢复出厂擦除）。
 *
 * 各业务服务（system/auth/clock/tls）仍各自负责自身键的读写与序列化，
 * 本组件仅集中登记「应用层 NVS 命名空间」清单并提供恢复出厂擦除，避免
 * 恢复出厂时遗漏某个命名空间（如管理密码 auth）。WiFi 射频校准位于
 * ESP-IDF 自有 NVS 区，不在此清单内，恢复出厂后予以保留。
 */

/** 应用层 NVS 命名空间清单（必须与各服务 nvs_open 所用命名空间一致）。 */
#define ESPAPERPLAY_NVS_NS_SYSTEM "system"
#define ESPAPERPLAY_NVS_NS_AUTH    "auth"
#define ESPAPERPLAY_NVS_NS_CLOCK   "clock"
#define ESPAPERPLAY_NVS_NS_TLS     "tls"

/**
 * @brief 恢复出厂：擦除全部应用层 NVS 命名空间。
 *
 * 逐个擦除已知应用命名空间（system/auth/clock/tls）。命名空间不存在时
 * 视为成功（ESP_ERR_NVS_NOT_FOUND 被忽略）。调用方通常随后重启设备，
 * 各服务在重启后从空 NVS 重新初始化为出厂默认。
 *
 * @return ESP_OK=全部擦除成功（含本就不存在的）；其他=某个命名空间擦除失败。
 */
esp_err_t espaperplay_nvs_factory_reset(void);

#ifdef __cplusplus
}
#endif
