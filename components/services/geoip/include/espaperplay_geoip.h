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
 * @file espaperplay_geoip.h
 * @brief IP 地理位置查询服务（功能 2：使用相同 IP 获取地理位置）。
 *
 * 通过 UAPI 平台（https://uapis.cn）的「IP 查询」接口
 * （GET https://uapis.cn/api/v1/network/ipinfo?ip=...）查询指定公网 IP 的
 * 地理位置（国家 / 省份 / 城市）、经纬度、运营商与所属机构信息。
 *
 * 默认携带 source=commercial 参数以获取更完整的信息（含时区字段
 * time_zone，供时钟服务设置时区使用）；商业级查询失败时自动回退到
 * 标准查询（此时 time_zone 为空）。
 *
 * 查询结果按 IP 缓存在内存中（默认 1 小时 TTL，最多
 * ESPAPERPLAY_GEOIP_CACHE_ENTRIES 条，满时淘汰最旧），TTL 内重复查询
 * 同一 IP 直接返回缓存，不发起网络请求；可通过
 * espaperplay_geoip_set_cache_ttl_ms() 调整有效期（设为 0 禁用缓存），
 * 或 espaperplay_geoip_cache_clear() 强制下次实时查询。
 *
 * 本组件独立可复用：调用方传入 IP（如来自 netip 服务），本组件只负责
 * 地理位置查询，不依赖其他服务。
 */

/** IP 字符串缓冲最大长度（含结尾 NUL，兼容 IPv6）。 */
#define ESPAPERPLAY_GEOIP_IP_MAX_LEN 46
/** 请求超时时间（毫秒）。 */
#define ESPAPERPLAY_GEOIP_HTTP_TIMEOUT_MS 10000
/** 响应体最大缓存（字节），超过则截断并视为异常。 */
#define ESPAPERPLAY_GEOIP_RESP_MAX_LEN 8192
/** 查询结果默认缓存有效期（毫秒）：1 小时。 */
#define ESPAPERPLAY_GEOIP_CACHE_TTL_MS (60 * 60 * 1000)
/** 缓存条目数（按 IP 区分，满时淘汰最旧条目）。 */
#define ESPAPERPLAY_GEOIP_CACHE_ENTRIES 4

/**
 * @brief IP 地理位置信息。
 */
typedef struct {
    char ip[ESPAPERPLAY_GEOIP_IP_MAX_LEN]; /*!< 查询的 IP 地址 */
    char region[128];                      /*!< 地理位置，格式：国家 省份 城市（如 "中国 北京 昌平"） */
    char isp[128];                         /*!< 运营商名称 */
    char llc[64];                          /*!< 所属机构（如 "电信"） */
    char asn[32];                          /*!< 自治系统号（如 "AS4847"） */
    char time_zone[64];                    /*!< IANA 时区（如 "Asia/Shanghai"），商业级查询可用，否则为空 */
    double latitude;                       /*!< 纬度 */
    double longitude;                      /*!< 经度 */
    bool has_coordinates;                  /*!< 是否成功解析出经纬度 */
} espaperplay_geoip_info_t;

/**
 * @brief 查询指定公网 IP 的地理位置信息。
 *
 * 先查内存缓存（按 IP）：TTL 内命中直接返回缓存结果；未命中或已过期时
 * 向 UAPI「IP 查询」接口发起 HTTPS 请求（优先商业级查询以获取时区，
 * 失败自动回退标准查询），解析并填充 info 中的各字段，成功后将结果写入
 * 缓存（缓存被禁用时不写入）。
 *
 * @param ip   要查询的公网 IP 地址（非空，如 "1.2.3.4"）。
 * @param info 输出地理位置信息（非空）。
 *
 * @return ESP_OK 查询成功；否则返回错误码
 *         （参数非法 / 网络 / TLS / HTTP 状态 / JSON 解析失败）。
 */
esp_err_t espaperplay_geoip_query(const char *ip, espaperplay_geoip_info_t *info);

/**
 * @brief 设置查询结果缓存有效期。
 *
 * 修改后立即生效，已缓存但超出新有效期的结果会在下次查询时视为过期。
 *
 * @param ttl_ms 缓存有效期（毫秒）；0 表示禁用缓存（每次实时查询）。
 */
void espaperplay_geoip_set_cache_ttl_ms(uint32_t ttl_ms);

/**
 * @brief 清空缓存。
 *
 * 调用后下一次查询强制发起实时请求；对已禁用缓存的场景无副作用。
 */
void espaperplay_geoip_cache_clear(void);

#ifdef __cplusplus
}
#endif
