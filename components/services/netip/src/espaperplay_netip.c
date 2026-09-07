/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_crt_bundle.h"
#include "nethttp.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "cJSON.h"

#include "espaperplay_netip.h"

static const char *TAG = "ESPaperPlay_NETIP";

/** UAPI「查询我的 IP」接口地址。 */
#define ESPAPERPLAY_NETIP_API_URL "https://uapis.cn/api/v1/network/myip"

/* ------------------------------------------------------------------ */
/* 结果缓存（TTL 内存缓存，避免重复请求）                                 */
/* ------------------------------------------------------------------ */

/** 缓存条目：本机公网 IP 唯一，单条即可。 */
typedef struct {
    char ip[ESPAPERPLAY_NETIP_IP_MAX_LEN]; /*!< 缓存的公网 IP */
    uint64_t ts_ms;                        /*!< 缓存写入时间戳（esp_timer，毫秒） */
    bool valid;                            /*!< 是否已有有效缓存 */
} netip_cache_t;

static netip_cache_t s_cache = {0};                     /*!< 查询结果缓存 */
static uint32_t s_cache_ttl_ms = ESPAPERPLAY_NETIP_CACHE_TTL_MS; /*!< 缓存有效期（毫秒），0 表示禁用 */
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED; /*!< 缓存访问锁 */

/** 当前时间（毫秒，esp_timer）。 */
static uint64_t netip_now_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/** 缓存是否命中（调用方需持有锁）。 */
static bool netip_cache_hit_locked(char *ip_out, size_t ip_out_len) {
    if (!s_cache.valid || s_cache_ttl_ms == 0) {
        return false;
    }
    if (netip_now_ms() - s_cache.ts_ms >= s_cache_ttl_ms) {
        return false; /* 已过期 */
    }
    strlcpy(ip_out, s_cache.ip, ip_out_len);
    return true;
}

/** 写入缓存（调用方需持有锁；仅成功查询后调用）。 */
static void netip_cache_store_locked(const char *ip) {
    if (s_cache_ttl_ms == 0) {
        return; /* 缓存被禁用 */
    }
    strlcpy(s_cache.ip, ip, sizeof(s_cache.ip));
    s_cache.ts_ms = netip_now_ms();
    s_cache.valid = true;
    ESP_LOGD(TAG, "cached \"%s\" for %u ms", s_cache.ip, s_cache_ttl_ms);
}

/* ------------------------------------------------------------------ */
/* HTTPS GET 辅助（本组件自包含，不与其他服务共享）                        */
/* ------------------------------------------------------------------ */

/** 发起一次 HTTPS GET（公共 nethttp 客户端；512B 起倍增，响应上限见宏）。 */
static esp_err_t netip_http_get(const char *url, char **out_body) {
    const nethttp_cfg_t cfg = {
        .url = url,
        .timeout_ms = ESPAPERPLAY_NETIP_HTTP_TIMEOUT_MS,
        .max_len = ESPAPERPLAY_NETIP_RESP_MAX_LEN,
    };
    return nethttp_get(&cfg, out_body, NULL);
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_netip_query(char *ip_out, size_t ip_out_len) {
    if (ip_out == NULL || ip_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 缓存命中：TTL 内直接返回，不发网络请求。 */
    portENTER_CRITICAL(&s_cache_lock);
    bool hit = netip_cache_hit_locked(ip_out, ip_out_len);
    portEXIT_CRITICAL(&s_cache_lock);
    if (hit) {
        ESP_LOGD(TAG, "cache hit: %s", ip_out);
        return ESP_OK;
    }

    char *body = NULL;
    esp_err_t err = netip_http_get(ESPAPERPLAY_NETIP_API_URL, &body);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGE(TAG, "failed to parse json: %s", body);
        free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *ip = cJSON_GetObjectItemCaseSensitive(root, "ip");
    if (cJSON_IsString(ip) && ip->valuestring != NULL && ip->valuestring[0] != '\0') {
        strlcpy(ip_out, ip->valuestring, ip_out_len);
        ESP_LOGI(TAG, "public ip: %s", ip_out);
        err = ESP_OK;
    } else {
        ESP_LOGE(TAG, "no \"ip\" field in response: %s", body);
        err = ESP_ERR_INVALID_RESPONSE;
    }

    cJSON_Delete(root);
    free(body);

    /* 查询成功才更新缓存；失败时保留旧缓存。 */
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_cache_lock);
        netip_cache_store_locked(ip_out);
        portEXIT_CRITICAL(&s_cache_lock);
    }
    return err;
}

void espaperplay_netip_set_cache_ttl_ms(uint32_t ttl_ms) {
    portENTER_CRITICAL(&s_cache_lock);
    s_cache_ttl_ms = ttl_ms;
    portEXIT_CRITICAL(&s_cache_lock);
    ESP_LOGI(TAG, "cache ttl set to %u ms (%s)", ttl_ms, ttl_ms == 0 ? "disabled" : "enabled");
}

void espaperplay_netip_cache_clear(void) {
    portENTER_CRITICAL(&s_cache_lock);
    s_cache.valid = false;
    portEXIT_CRITICAL(&s_cache_lock);
    ESP_LOGI(TAG, "cache cleared");
}
