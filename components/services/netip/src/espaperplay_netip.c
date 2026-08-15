/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "cJSON.h"

#include "espaperplay_netip.h"

static const char *TAG = "ESPaperPlay_NETIP";

/** UAPI「查询我的 IP」接口地址。 */
#define ESPAPERPLAY_NETIP_API_URL "https://uapis.cn/api/v1/network/myip"

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
    return err;
}
