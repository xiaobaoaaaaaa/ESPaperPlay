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
#include "esp_http_client.h"
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

/** 响应累积缓冲。 */
typedef struct {
    char *data; /*!< 已接收的响应体（NUL 结尾） */
    size_t len; /*!< 已接收字节数（不含结尾 NUL） */
    size_t cap; /*!< 已分配容量 */
} netip_resp_t;

/** esp_http_client 事件回调：把响应体分块累积进缓冲。 */
static esp_err_t netip_http_event_handler(esp_http_client_event_t *evt) {
    netip_resp_t *resp = (netip_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t need = resp->len + evt->data_len + 1;
        if (need > ESPAPERPLAY_NETIP_RESP_MAX_LEN) {
            ESP_LOGW(TAG, "response too large (%u bytes), aborting", (unsigned)need);
            return ESP_FAIL;
        }
        if (need > resp->cap) {
            size_t new_cap = resp->cap ? resp->cap : 512;
            while (new_cap < need) {
                new_cap *= 2;
            }
            char *new_data = realloc(resp->data, new_cap);
            if (new_data == NULL) {
                return ESP_FAIL;
            }
            resp->data = new_data;
            resp->cap = new_cap;
        }
        memcpy(resp->data + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->data[resp->len] = '\0';
    }
    return ESP_OK;
}

/**
 * @brief 发起一次 HTTPS GET 请求并返回完整响应体。
 *
 * 使用 ESP-IDF 内置 CA 证书包（esp_crt_bundle）校验服务器证书。
 *
 * @param url        请求地址（非空）。
 * @param out_body   成功时输出 malloc 的响应体字符串（调用方负责 free）。
 *
 * @return ESP_OK 且 HTTP 状态为 200 时成功，否则返回错误码。
 */
static esp_err_t netip_http_get(const char *url, char **out_body) {
    netip_resp_t resp = {0};

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = ESPAPERPLAY_NETIP_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .event_handler = netip_http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "failed to init http client");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http request failed: %s", esp_err_to_name(err));
        free(resp.data);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "unexpected http status: %d", status);
        free(resp.data);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (resp.data == NULL || resp.len == 0) {
        ESP_LOGE(TAG, "empty response body");
        free(resp.data);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_body = resp.data;
    return ESP_OK;
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
