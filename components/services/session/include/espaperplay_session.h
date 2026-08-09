/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_session.h
 * @brief 会话管理服务。
 *
 * 管理登录后的会话生命周期（签发 / 校验 / 续期 / 吊销 / 过期清理），
 * 并承载登录准入控制（失败限速与锁定），供 Web 服务及其他需要
 * "已登录"语义的模块复用。
 *
 * 会话表保存在内存（RAM）中，掉电 / 重启后全部失效（需重新登录），
 * 符合"仅防远程攻击者"的威胁模型，也避免频繁写 NVS 造成 flash 磨损。
 * token 以硬件随机数签发，服务端仅保存其 SHA-256 哈希，校验用恒定时间
 * 比较；登录失败连续超阈值后进入锁定期（期间拒绝一切登录尝试）。
 */

/** 会话表最大并发会话数。 */
#define ESPAPERPLAY_SESSION_MAX 8
/** 随机 token 长度（字节，128bit 熵）。 */
#define ESPAPERPLAY_SESSION_TOKEN_LEN 16
/** token 的 hex 编码长度（含结尾 '\0'）。 */
#define ESPAPERPLAY_SESSION_TOKEN_HEX_LEN (ESPAPERPLAY_SESSION_TOKEN_LEN * 2 + 1)

/** 会话默认有效期（毫秒，1 小时）。 */
#define ESPAPERPLAY_SESSION_DEFAULT_TTL_MS 3600000
/** 过期会话周期清理间隔（毫秒）。 */
#define ESPAPERPLAY_SESSION_CLEANUP_PERIOD_MS 60000

/** 连续登录失败锁定阈值（达到后进入锁定期）。 */
#define ESPAPERPLAY_SESSION_LOGIN_MAX_FAILURES 5
/** 登录锁定持续时间（毫秒，5 分钟）。 */
#define ESPAPERPLAY_SESSION_LOGIN_LOCKOUT_MS 300000

/** 会话 ID（服务端内部标识）。 */
typedef uint32_t espaperplay_session_id_t;

/**
 * @brief 初始化会话管理服务。
 *
 * 初始化 PSA 密码学库、创建互斥锁，并启动过期会话周期清理定时器。
 * 应在使用会话功能之前调用，且仅需调用一次。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_session_init(void);

/**
 * @brief 签发一个新会话（登录成功后调用）。
 *
 * 生成随机 token（hex 编码写入 token_buf，调用方下发给客户端），服务端
 * 仅保存其 SHA-256 哈希。会话表已满时踢掉最久未活跃的会话。
 *
 * @param ttl_ms         会话有效期（毫秒），0 表示使用默认值。
 * @param token_buf      接收 hex 编码 token 的缓冲区。
 * @param token_buf_size token_buf 容量（≥ ESPAPERPLAY_SESSION_TOKEN_HEX_LEN）。
 * @param out_id         返回会话 ID（可传 NULL）。
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；
 *         内部错误返回对应错误码。
 */
esp_err_t espaperplay_session_create(uint32_t ttl_ms, char *token_buf, size_t token_buf_size,
                                     espaperplay_session_id_t *out_id);

/**
 * @brief 校验 token 是否有效。
 *
 * 恒定时间哈希比较；已过期或不在表中的 token 视为无效。校验成功会刷新
 * 会话的最近活跃时间（滑动过期）。
 *
 * @param token  hex 编码的 token。
 * @param out_id 返回会话 ID（可传 NULL）。
 * @return ESP_OK 有效；ESP_ERR_INVALID_ARG 参数非法；
 *         ESP_ERR_NOT_FOUND token 无效或已过期。
 */
esp_err_t espaperplay_session_verify(const char *token, espaperplay_session_id_t *out_id);

/**
 * @brief 续期会话有效期。
 *
 * @param token  hex 编码的 token。
 * @param ttl_ms 新的有效期（毫秒），0 表示使用默认值。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法；
 *         ESP_ERR_NOT_FOUND token 无效或已过期。
 */
esp_err_t espaperplay_session_renew(const char *token, uint32_t ttl_ms);

/**
 * @brief 吊销会话（登出）。
 *
 * token 不存在时也视为成功（幂等），便于登出接口直接调用。
 *
 * @param token hex 编码的 token。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法。
 */
esp_err_t espaperplay_session_destroy(const char *token);

/**
 * @brief 主动清理所有已过期会话。
 *
 * 通常由内部周期定时器调用；外部也可手动触发。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t espaperplay_session_cleanup(void);

/**
 * @brief 当前有效会话数。
 *
 * @return 有效会话数量（0 ~ ESPAPERPLAY_SESSION_MAX）。
 */
size_t espaperplay_session_count(void);

/**
 * @brief 吊销所有会话（如修改密码后强制重新登录）。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t espaperplay_session_clear_all(void);

/**
 * @brief 是否允许发起一次登录尝试。
 *
 * 返回 false 表示当前处于登录锁定期，调用方应直接拒绝（如 HTTP 429 +
 * Retry-After）且不再执行密码校验（节省 CPU，同时防暴力破解与 DoS）。
 *
 * @return 允许返回 true，锁定返回 false。
 */
bool espaperplay_session_login_allowed(void);

/**
 * @brief 登记一次登录失败。
 *
 * 累计失败次数，达到阈值后进入锁定期。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t espaperplay_session_login_failure(void);

/**
 * @brief 登记一次登录成功。
 *
 * 清零失败计数并解除锁定。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t espaperplay_session_login_success(void);

#ifdef __cplusplus
}
#endif
