#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "vars.h"

#include "yiyan.h"

#define TAG  "yiyan"

//解析一言内容
void parse_yiyan(const char *response)
{
    cJSON *json = cJSON_Parse(response);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    cJSON *hitokoto = cJSON_GetObjectItem(json, "hitokoto");
    if (cJSON_IsString(hitokoto) && (hitokoto->valuestring != NULL)) {
        ESP_LOGI(TAG, "Hitokoto: %s", hitokoto->valuestring);
        set_var_yiyan(hitokoto->valuestring); // 保存到全局变量
    } else {
        ESP_LOGE(TAG, "Hitokoto not found or not a string");
    }

    cJSON_Delete(json);
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    static char *response_buf = NULL;
    static int total_len = 0;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            total_len = 0;
            free(response_buf); // 清理上一次的
            response_buf = NULL;
            break;

        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                response_buf = realloc(response_buf, total_len + evt->data_len + 1);
                memcpy(response_buf + total_len, evt->data, evt->data_len);
                total_len += evt->data_len;
                response_buf[total_len] = '\0';  // null 终止
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            if (response_buf) {
                // 传给解析函数
                parse_yiyan(response_buf);
                free(response_buf);
                response_buf = NULL;
                total_len = 0;
            }
            break;

        default:
            break;
    }

    return ESP_OK;
}

void get_yiyan(void)
{
    // 判断是否已经连接到WiFi
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi AP info: %s", esp_err_to_name(ret));
        return;
    }

    // 建立 HTTP 客户端配置
    esp_http_client_config_t config = {
        .url = "https://v1.hitokoto.cn/",
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // 发送 GET 请求
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %lld",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}
