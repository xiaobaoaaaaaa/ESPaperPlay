/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - 公共 HTTPS GET 客户端（实现）
 * 见 include/nethttp.h 的接口说明。
 */
#include "nethttp.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ESPaperPlay_NETHTTP";

/** 响应累积缓冲（prealloc=false 时按倍增 realloc 增长）。 */
typedef struct {
    char *data;      /*!< 响应体缓冲（NUL 结尾） */
    size_t len;      /*!< 已接收字节数（不含结尾 NUL） */
    size_t cap;      /*!< 已分配容量（prealloc=true 时等于 max） */
    size_t max;      /*!< 允许的最大字节数 */
    bool prealloc;   /*!< 一次性分配模式 */
} nethttp_resp_t;

/** esp_http_client 事件回调：把响应体分块累积进缓冲。 */
static esp_err_t nethttp_event_handler(esp_http_client_event_t *evt) {
    nethttp_resp_t *resp = (nethttp_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        const size_t need = resp->len + (size_t)evt->data_len + 1;
        if (need > resp->max) {
            ESP_LOGW(TAG, "response too large (%u bytes), aborting", (unsigned)need);
            return ESP_FAIL;
        }
        if (need > resp->cap) {
            if (resp->prealloc) {
                return ESP_FAIL; /* 预分配模式下不应出现，防御 */
            }
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
        memcpy(resp->data + resp->len, evt->data, (size_t)evt->data_len);
        resp->len += (size_t)evt->data_len;
        resp->data[resp->len] = '\0';
    }
    return ESP_OK;
}

/** 瞬时连接失败判定（重试 only 对这些生效；4xx/5xx 属确定性结果不重试）。 */
static bool nethttp_err_transient(esp_err_t err) {
    return err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_HTTP_CONNECTION_CLOSED ||
           err == ESP_ERR_TIMEOUT;
}

esp_err_t nethttp_get(const nethttp_cfg_t *cfg, char **out_body, size_t *out_len) {
    if (cfg == NULL || cfg->url == NULL || cfg->url[0] == '\0' || cfg->max_len == 0 ||
        out_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t timeout_ms =
        cfg->timeout_ms != 0 ? cfg->timeout_ms : NETHTTP_TIMEOUT_MS_DEFAULT;

    for (int attempt = 0;; attempt++) {
        nethttp_resp_t resp = {0};
        resp.max = cfg->max_len;
        resp.prealloc = cfg->prealloc;
        if (cfg->prealloc) {
            /* 响应缓冲按上限一次性分配（>=8KB 大缓冲随
             * SPIRAM_MALLOC_ALWAYSINTERNAL 阈值自动落入 PSRAM）。 */
            resp.cap = cfg->max_len;
            resp.data = malloc(cfg->max_len + 1);
        } else {
            resp.data = NULL; /* 事件回调 512B 起倍增 */
        }
        if (resp.data == NULL && cfg->prealloc) {
            ESP_LOGE(TAG, "failed to allocate response buffer (%u bytes)",
                     (unsigned)cfg->max_len);
            return ESP_ERR_NO_MEM;
        }
        if (resp.data != NULL) {
            resp.data[0] = '\0';
        }

        esp_http_client_config_t http_cfg = {
            .url = cfg->url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = (int)timeout_ms,
            .disable_auto_redirect = true,
            .event_handler = nethttp_event_handler,
            .user_data = &resp,
        };
        if (cfg->cert_pem != NULL) {
            /* 站点专用 CA：目标链不被全局证书包收录时以它校验
             * （esp_tls 中 cert_pem 与 crt_bundle_attach 互斥）。 */
            http_cfg.cert_pem = cfg->cert_pem;
        } else {
            http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (client == NULL) {
            ESP_LOGE(TAG, "failed to init http client");
            free(resp.data);
            return ESP_ERR_NO_MEM;
        }
        if (cfg->headers != NULL) {
            for (int i = 0; cfg->headers[i] != NULL && cfg->headers[i + 1] != NULL; i += 2) {
                esp_http_client_set_header(client, cfg->headers[i], cfg->headers[i + 1]);
            }
        }

        const esp_err_t err = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        /* 瞬时连接失败：退避重试（自愈 LWIP 连接池暂满 / 网络抖动等场景）。 */
        if (err != ESP_OK && nethttp_err_transient(err) && attempt < cfg->max_retries) {
            ESP_LOGW(TAG, "http request failed (%s), retrying %d/%d", esp_err_to_name(err),
                     attempt + 1, cfg->max_retries);
            vTaskDelay(pdMS_TO_TICKS(300u << attempt));
            free(resp.data);
            continue;
        }

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
        if (out_len != NULL) {
            *out_len = resp.len;
        }
        return ESP_OK;
    }
}
