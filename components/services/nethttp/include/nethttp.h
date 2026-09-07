/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - 公共 HTTPS GET 客户端
 *
 * 此前 geoip / netip / weather 三处各写一份完整的
 * 「init → 事件回调累积响应体 → perform → 状态检查 → 清理」流程：
 * geoip 与 netip 逐行级相同；weather 在其上增强（预分配缓冲、瞬时失败
 * 重试、自定义头、gzip 解压）。本模块合并为单一实现，能力以配置表达，
 * gzip 解压等仍留在各服务侧（载荷格式属于业务）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 默认超时（毫秒）：cfg.timeout_ms 传 0 时生效。 */
#define NETHTTP_TIMEOUT_MS_DEFAULT 10000

/** HTTPS GET 请求配置。 */
typedef struct {
    const char *url;             /*!< 请求地址（非空） */
    uint32_t timeout_ms;         /*!< 超时毫秒；0 = NETHTTP_TIMEOUT_MS_DEFAULT */
    size_t max_len;              /*!< 响应体上限（字节，非空） */
    bool prealloc;               /*!< true=按上限一次性分配（大缓冲随
                                      SPIRAM_MALLOC_ALWAYSINTERNAL 落 PSRAM，
                                      避免 realloc 增长在内部 RAM 制造碎片）；
                                      false=512B 起倍增增长（小响应省内存） */
    const char *const *headers;  /*!< 附加请求头 {"K","V",...}，NULL 结尾；可 NULL */
    int max_retries;             /*!< 瞬时失败最大重试次数（0=不重试；退避 300ms<<n） */
} nethttp_cfg_t;

/**
 * @brief 发起一次 HTTPS GET 请求并返回完整响应体。
 *
 * 使用 ESP-IDF 内置 CA 证书包（esp_crt_bundle）校验服务器证书，禁用自动
 * 重定向。瞬时连接失败（connect 被拒 / 连接被对端关闭 / 超时）按
 * max_retries 自动重试。
 *
 * @param cfg      请求配置（非空）。
 * @param out_body 成功时输出 malloc 的响应体（NUL 结尾，调用方负责 free）。
 * @param out_len  可空；成功时输出响应体长度（不含 NUL）。
 *
 * @return ESP_OK 且 HTTP 状态为 200 时成功，否则返回错误码（响应体未输出）。
 */
esp_err_t nethttp_get(const nethttp_cfg_t *cfg, char **out_body, size_t *out_len);

#ifdef __cplusplus
}
#endif
