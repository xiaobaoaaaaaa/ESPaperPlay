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
 * @file espaperplay_auth.h
 * @brief 设备鉴权服务。
 *
 * 提供统一的设备密码管理：安全存储、校验与更改。供 Web 管理控制台及
 * 其他需要身份鉴权的模块（服务）复用。
 *
 * 密码绝不落盘明文：NVS 中仅保存"随机盐 + PBKDF2 迭代次数 + 派生密钥"。
 * 校验时按存储参数重新派生并做恒定时间比较，抗时序侧信道；PBKDF2
 * 迭代拉伸提高对离线暴力破解的成本。
 */

/** NVS 配置命名空间。 */
#define ESPAPERPLAY_AUTH_NVS_NAMESPACE "auth"

/** 密码最小长度（不含结尾 '\0'）。 */
#define ESPAPERPLAY_AUTH_PASSWORD_MIN_LEN 8
/** 密码最大长度（含结尾 '\0'）。 */
#define ESPAPERPLAY_AUTH_PASSWORD_MAX_LEN 64

/** 随机盐长度（字节）。 */
#define ESPAPERPLAY_AUTH_SALT_LEN 16
/** PBKDF2 派生密钥长度（字节，HMAC-SHA256 输出）。 */
#define ESPAPERPLAY_AUTH_DERIVED_LEN 32
/** PBKDF2-HMAC-SHA256 迭代次数（越大越安全，单次校验耗时随之增加）。
 *  实测 ESP32-S3 硬件 SHA 驱动约 270us/次：10000 次约 2.7s（过慢，易触发
 *  Task WDT），2000 次约 0.5s，为安全与实时性的折中；记录自带迭代次数，
 *  调整后旧记录仍可正确校验。 */
#define ESPAPERPLAY_AUTH_PBKDF2_ITERATIONS 2000
/** NVS 密码记录格式版本。 */
#define ESPAPERPLAY_AUTH_BLOB_VERSION 1

/**
 * @brief 初始化鉴权服务。
 *
 * 初始化 NVS 与 PSA 密码学库并从 NVS 加载密码记录；出厂状态不设置密码
 * （espaperplay_auth_is_configured() 返回 false），需通过
 * espaperplay_auth_change_password() 设置。应在其他使用鉴权的模块之前
 * 调用，且仅需调用一次。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_auth_init(void);

/**
 * @brief 是否已配置密码。
 *
 * @return 已配置返回 true；NVS 记录缺失或损坏时返回 false。
 */
bool espaperplay_auth_is_configured(void);

/**
 * @brief 校验密码是否正确。
 *
 * 按存储的盐与迭代次数重新派生输入密码，再做恒定时间比较。
 * 该函数为 CPU 密集操作（PBKDF2 迭代拉伸），耗时与迭代次数成正比；
 * 调用方（如 Web 会话处理器）应避免在实时性敏感路径中调用。
 *
 * @param password 待校验的密码明文。
 * @return ESP_OK 密码正确；ESP_ERR_INVALID_ARG 参数非法；
 *         ESP_ERR_NOT_FOUND 尚未配置密码；ESP_ERR_NOT_ALLOWED 密码错误；
 *         内部错误返回对应错误码。
 */
esp_err_t espaperplay_auth_verify(const char *password);

/**
 * @brief 设置或更改密码。
 *
 * 生成新的随机盐并按当前迭代次数派生后写回 NVS；
 * 新密码即刻生效，旧密码立即失效。
 *
 * @param new_password 新密码明文。
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG /
 *         ESP_ERR_INVALID_SIZE；NVS 写入失败返回错误码。
 */
esp_err_t espaperplay_auth_change_password(const char *new_password);

/**
 * @brief 清除密码（恢复"未配置"状态）。
 *
 * 删除 NVS 中的密码记录，之后 espaperplay_auth_is_configured() 返回 false，
 * 设备回到出厂免鉴权状态。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_auth_clear_password(void);

#ifdef __cplusplus
}
#endif
