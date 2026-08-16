/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_netip.h
 * @brief 本机公网 IP 查询服务（功能 1：获取本机 IP 地址）。
 *
 * 通过 UAPI 平台（https://uapis.cn）的「查询我的 IP」接口
 * （GET https://uapis.cn/api/v1/network/myip）获取本机出口公网 IP。
 * 该接口以调用方（即设备）的出口 IP 为查询对象，无需任何参数。
 *
 * 查询结果缓存在内存中（默认 1 小时 TTL），TTL 内重复调用直接返回缓存，
 * 不发起网络请求；可通过 espaperplay_netip_set_cache_ttl_ms() 调整有效期
 * （设为 0 禁用缓存），或 espaperplay_netip_cache_clear() 强制下次实时查询。
 *
 * 本组件独立可复用：只负责「获取 IP」，不依赖地理位置 / 时间等其他服务。
 */

/** 公网 IP 字符串缓冲最大长度（含结尾 NUL，兼容 IPv6）。 */
#define ESPAPERPLAY_NETIP_IP_MAX_LEN 46
/** 请求超时时间（毫秒）。 */
#define ESPAPERPLAY_NETIP_HTTP_TIMEOUT_MS 10000
/** 响应体最大缓存（字节），超过则截断并视为异常。 */
#define ESPAPERPLAY_NETIP_RESP_MAX_LEN 4096
/** 查询结果默认缓存有效期（毫秒）：1 小时。 */
#define ESPAPERPLAY_NETIP_CACHE_TTL_MS (60 * 60 * 1000)

/**
 * @brief 查询本机公网 IP 地址。
 *
 * 先查内存缓存：TTL 内命中直接返回缓存结果；未命中或已过期时向 UAPI
 * 「查询我的 IP」接口发起 HTTPS 请求，解析响应中的 "ip" 字段，成功后将
 * 结果写入缓存（缓存被禁用时不写入）。该接口返回调用方（设备）的出口
 * 公网 IP。
 *
 * @param ip_out    输出缓冲区（非空），用于存放 IP 字符串（如 "1.2.3.4"）。
 * @param ip_out_len 输出缓冲区大小，建议 >= ESPAPERPLAY_NETIP_IP_MAX_LEN。
 *
 * @return ESP_OK 查询成功；否则返回错误码
 *         （网络 / TLS / HTTP 状态 / JSON 解析失败均会返回非 ESP_OK）。
 */
esp_err_t espaperplay_netip_query(char *ip_out, size_t ip_out_len);

/**
 * @brief 设置查询结果缓存有效期。
 *
 * 修改后立即生效，已缓存但超出新有效期的结果会在下次查询时视为过期。
 *
 * @param ttl_ms 缓存有效期（毫秒）；0 表示禁用缓存（每次实时查询）。
 */
void espaperplay_netip_set_cache_ttl_ms(uint32_t ttl_ms);

/**
 * @brief 清空缓存。
 *
 * 调用后下一次查询强制发起实时请求；对已禁用缓存的场景无副作用。
 */
void espaperplay_netip_cache_clear(void);

#ifdef __cplusplus
}
#endif
