/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

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
 * 本组件独立可复用：调用方传入 IP（如来自 netip 服务），本组件只负责
 * 地理位置查询，不依赖其他服务。
 */

/** IP 字符串缓冲最大长度（含结尾 NUL，兼容 IPv6）。 */
#define ESPAPERPLAY_GEOIP_IP_MAX_LEN 46
/** 请求超时时间（毫秒）。 */
#define ESPAPERPLAY_GEOIP_HTTP_TIMEOUT_MS 10000
/** 响应体最大缓存（字节），超过则截断并视为异常。 */
#define ESPAPERPLAY_GEOIP_RESP_MAX_LEN 8192

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
 * 向 UAPI「IP 查询」接口发起 HTTPS 请求（优先商业级查询以获取时区，
 * 失败自动回退标准查询），解析并填充 info 中的各字段。
 *
 * @param ip   要查询的公网 IP 地址（非空，如 "1.2.3.4"）。
 * @param info 输出地理位置信息（非空）。
 *
 * @return ESP_OK 查询成功；否则返回错误码
 *         （参数非法 / 网络 / TLS / HTTP 状态 / JSON 解析失败）。
 */
esp_err_t espaperplay_geoip_query(const char *ip, espaperplay_geoip_info_t *info);

#ifdef __cplusplus
}
#endif
