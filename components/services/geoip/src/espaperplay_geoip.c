/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

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

#include "espaperplay_geoip.h"

static const char *TAG = "ESPaperPlay_GEOIP";

/** UAPI「IP 查询」接口地址。 */
#define ESPAPERPLAY_GEOIP_API_URL "https://uapis.cn/api/v1/network/ipinfo"

/* ------------------------------------------------------------------ */
/* 结果缓存（TTL 内存缓存，按 IP 区分，避免重复请求）                     */
/* ------------------------------------------------------------------ */

/** 缓存条目：一个 IP 对应一份地理位置信息。 */
typedef struct {
    char ip[ESPAPERPLAY_GEOIP_IP_MAX_LEN]; /*!< 查询的 IP */
    espaperplay_geoip_info_t info;         /*!< 缓存的地理位置信息 */
    uint64_t ts_ms;                        /*!< 缓存写入时间戳（esp_timer，毫秒） */
    bool valid;                            /*!< 是否已有有效缓存 */
} geoip_cache_entry_t;

static geoip_cache_entry_t s_cache[ESPAPERPLAY_GEOIP_CACHE_ENTRIES];          /*!< 查询结果缓存 */
static uint32_t s_cache_ttl_ms = ESPAPERPLAY_GEOIP_CACHE_TTL_MS;              /*!< 缓存有效期（毫秒），0 表示禁用 */
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;              /*!< 缓存访问锁 */

/** 当前时间（毫秒，esp_timer）。 */
static uint64_t geoip_now_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/** 按 IP 查找未过期缓存；命中时输出信息并返回 true（调用方需持有锁）。 */
static bool geoip_cache_lookup_locked(const char *ip, espaperplay_geoip_info_t *info) {
    if (s_cache_ttl_ms == 0) {
        return false; /* 缓存被禁用 */
    }
    for (int i = 0; i < ESPAPERPLAY_GEOIP_CACHE_ENTRIES; i++) {
        if (s_cache[i].valid && strcmp(s_cache[i].ip, ip) == 0 &&
            geoip_now_ms() - s_cache[i].ts_ms < s_cache_ttl_ms) {
            *info = s_cache[i].info;
            return true;
        }
    }
    return false;
}

/** 写入缓存：覆盖同 IP 条目，否则用空槽，满时淘汰最旧条目（调用方需持有锁）。 */
static void geoip_cache_store_locked(const char *ip, const espaperplay_geoip_info_t *info) {
    if (s_cache_ttl_ms == 0) {
        return; /* 缓存被禁用 */
    }

    int slot = -1;
    uint64_t oldest_ts = UINT64_MAX;
    for (int i = 0; i < ESPAPERPLAY_GEOIP_CACHE_ENTRIES; i++) {
        if (s_cache[i].valid && strcmp(s_cache[i].ip, ip) == 0) {
            slot = i; /* 同 IP：直接覆盖 */
            break;
        }
        if (!s_cache[i].valid) {
            slot = i; /* 空槽 */
            break;
        }
        if (s_cache[i].ts_ms < oldest_ts) {
            oldest_ts = s_cache[i].ts_ms;
            slot = i; /* 最旧条目（FIFO 淘汰备选） */
        }
    }
    if (slot < 0) {
        return; /* 理论不可达 */
    }

    strlcpy(s_cache[slot].ip, ip, sizeof(s_cache[slot].ip));
    s_cache[slot].info = *info;
    s_cache[slot].ts_ms = geoip_now_ms();
    s_cache[slot].valid = true;
    ESP_LOGD(TAG, "cached \"%s\" for %u ms", ip, s_cache_ttl_ms);
}

/* ------------------------------------------------------------------ */
/* HTTPS GET 辅助（本组件自包含，不与其他服务共享）                        */
/* ------------------------------------------------------------------ */

/** 响应累积缓冲。 */
typedef struct {
    char *data; /*!< 已接收的响应体（NUL 结尾） */
    size_t len; /*!< 已接收字节数（不含结尾 NUL） */
    size_t cap; /*!< 已分配容量 */
} geoip_resp_t;

/** esp_http_client 事件回调：把响应体分块累积进缓冲。 */
static esp_err_t geoip_http_event_handler(esp_http_client_event_t *evt) {
    geoip_resp_t *resp = (geoip_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t need = resp->len + evt->data_len + 1;
        if (need > ESPAPERPLAY_GEOIP_RESP_MAX_LEN) {
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
static esp_err_t geoip_http_get(const char *url, char **out_body) {
    geoip_resp_t resp = {0};

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = ESPAPERPLAY_GEOIP_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .event_handler = geoip_http_event_handler,
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
/* JSON 解析                                                            */
/* ------------------------------------------------------------------ */

/** 从 JSON 根节点复制字符串字段到目标缓冲（字段缺失时保持原值）。 */
static void geoip_copy_str_field(const cJSON *root, const char *field, char *dst, size_t dst_len) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(dst, item->valuestring, dst_len);
    }
}

/** 解析一次 ipinfo 响应体并填充 info。返回 ESP_OK 表示解析出至少一个有效字段。 */
static esp_err_t geoip_parse_body(const char *body, espaperplay_geoip_info_t *info) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGE(TAG, "failed to parse json: %s", body);
        return ESP_ERR_INVALID_RESPONSE;
    }

    geoip_copy_str_field(root, "ip", info->ip, sizeof(info->ip));
    geoip_copy_str_field(root, "region", info->region, sizeof(info->region));
    geoip_copy_str_field(root, "isp", info->isp, sizeof(info->isp));
    geoip_copy_str_field(root, "llc", info->llc, sizeof(info->llc));
    geoip_copy_str_field(root, "asn", info->asn, sizeof(info->asn));
    geoip_copy_str_field(root, "time_zone", info->time_zone, sizeof(info->time_zone));

    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(root, "latitude");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(root, "longitude");
    if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        info->latitude = lat->valuedouble;
        info->longitude = lon->valuedouble;
        info->has_coordinates = true;
    }

    cJSON_Delete(root);

    if (info->region[0] == '\0' && info->ip[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_geoip_query(const char *ip, espaperplay_geoip_info_t *info) {
    if (ip == NULL || ip[0] == '\0' || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 缓存命中：TTL 内直接返回，不发网络请求。 */
    portENTER_CRITICAL(&s_cache_lock);
    bool hit = geoip_cache_lookup_locked(ip, info);
    portEXIT_CRITICAL(&s_cache_lock);
    if (hit) {
        ESP_LOGD(TAG, "cache hit: %s", ip);
        return ESP_OK;
    }

    memset(info, 0, sizeof(*info));

    /* 优先商业级查询：返回更完整的地理信息（含时区 time_zone）。 */
    char url[192];
    snprintf(url, sizeof(url), "%s?ip=%s&source=commercial", ESPAPERPLAY_GEOIP_API_URL, ip);

    char *body = NULL;
    esp_err_t err = geoip_http_get(url, &body);
    if (err != ESP_OK) {
        /* 回退到标准查询（无 source 参数，不含时区字段）。 */
        ESP_LOGW(TAG, "commercial query failed (%s), falling back to standard query",
                 esp_err_to_name(err));
        snprintf(url, sizeof(url), "%s?ip=%s", ESPAPERPLAY_GEOIP_API_URL, ip);
        err = geoip_http_get(url, &body);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = geoip_parse_body(body, info);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "ip=%s region=\"%s\" isp=\"%s\" asn=%s tz=\"%s\" (%.4f, %.4f)", info->ip,
             info->region, info->isp, info->asn, info->time_zone, info->latitude, info->longitude);

    /* 查询成功才更新缓存；失败时保留旧缓存。 */
    portENTER_CRITICAL(&s_cache_lock);
    geoip_cache_store_locked(ip, info);
    portEXIT_CRITICAL(&s_cache_lock);
    return ESP_OK;
}

void espaperplay_geoip_set_cache_ttl_ms(uint32_t ttl_ms) {
    portENTER_CRITICAL(&s_cache_lock);
    s_cache_ttl_ms = ttl_ms;
    portEXIT_CRITICAL(&s_cache_lock);
    ESP_LOGI(TAG, "cache ttl set to %u ms (%s)", ttl_ms, ttl_ms == 0 ? "disabled" : "enabled");
}

void espaperplay_geoip_cache_clear(void) {
    portENTER_CRITICAL(&s_cache_lock);
    for (int i = 0; i < ESPAPERPLAY_GEOIP_CACHE_ENTRIES; i++) {
        s_cache[i].valid = false;
    }
    portEXIT_CRITICAL(&s_cache_lock);
    ESP_LOGI(TAG, "cache cleared");
}
