/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - 一言（Hitokoto）服务（实现）
 * 见 include/espaperplay_hitokoto.h 的接口说明。
 *
 * 持久化：每次成功获取后把快照整体写入 NVS（命名空间登记于
 * espaperplay_nvs.h）；启动时先恢复上次内容——设备昼夜睡眠、网络窗口
 * 碎片化，若不做持久化，"取到句子前就已入睡"的场景会长期回退问候语。
 */
#include "espaperplay_hitokoto.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "nethttp.h"
#include "nvs.h"

#include "espaperplay_nvs.h"
#include "espaperplay_wifi.h"

#include "espaperplay_hitokoto_cert.h"

static const char *TAG = "ESPaperPlay_HITOKOTO";

/* ------------------------------------------------------------------ */
/* 配置常量                                                             */
/* ------------------------------------------------------------------ */

/** 接口地址与请求参数：句长上限 32 字；分类取海报气质的六类
 * （动画/漫画/文学/影视/诗词/哲学，避开抖机灵等碎片向分类）。 */
#define HITOKOTO_API_URL "https://v1.hitokoto.cn/"
#define HITOKOTO_MAX_LENGTH 32
#define HITOKOTO_CATEGORIES "abdhik"

/** 响应体上限（接口单句 JSON 约 300 字节，留足余量防异常长响应）。 */
#define HITOKOTO_RESP_MAX 2048
/** 后台任务栈大小（nethttp/TLS 握手开销；结构体小，全静态分配）。 */
#define HITOKOTO_TASK_STACK_SIZE 12288
/** 后台任务优先级（与天气任务同级，均为低速后台拉取）。 */
#define HITOKOTO_TASK_PRIORITY 3
/** 启动时等待 STA 联网的最长时间（毫秒）。 */
#define HITOKOTO_WIFI_WAIT_MS 60000
/** WiFi 状态轮询间隔（毫秒）。 */
#define HITOKOTO_WIFI_POLL_MS 1000
/** 瞬时失败重试次数（nethttp 内退避）。 */
#define HITOKOTO_HTTP_MAX_RETRIES 2
/** 快照无效时的重试等待（毫秒）：设备睡眠期间 WiFi 断开、拉取必然
 * 失败，此间隔只在清醒时起效——缩短从"取到第一条"的等待。 */
#define HITOKOTO_INVALID_RETRY_MS 15000
/** NVS 键：最近一次成功获取的快照。 */
#define HITOKOTO_NVS_KEY_LAST "last"

/* ------------------------------------------------------------------ */
/* 全局状态                                                             */
/* ------------------------------------------------------------------ */

static SemaphoreHandle_t s_lock = NULL; /*!< 快照 / 任务句柄访问互斥锁 */
static TaskHandle_t s_task = NULL;      /*!< 后台刷新任务句柄（NULL=未运行） */
static espaperplay_hitokoto_t s_snap;   /*!< 当前一言快照（s_lock 保护） */
static volatile uint32_t s_interval_ms = ESPAPERPLAY_HITOKOTO_REFRESH_INTERVAL_MS;

/* ------------------------------------------------------------------ */
/* 持久化（NVS）                                                        */
/* ------------------------------------------------------------------ */

/** 把有效快照整体写入 NVS（结构体为定长 POD，直接按 blob 存取）。 */
static void hitokoto_persist(const espaperplay_hitokoto_t *h) {
    nvs_handle_t handle;
    if (nvs_open(ESPAPERPLAY_NVS_NS_HITOKOTO, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(handle, HITOKOTO_NVS_KEY_LAST, h, sizeof(*h)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

/** 启动时恢复上次快照（无有效记录时保持无效态，走问候语回退）。 */
static void hitokoto_restore(void) {
    nvs_handle_t handle;
    if (nvs_open(ESPAPERPLAY_NVS_NS_HITOKOTO, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    espaperplay_hitokoto_t saved;
    size_t len = sizeof(saved);
    if (nvs_get_blob(handle, HITOKOTO_NVS_KEY_LAST, &saved, &len) == ESP_OK &&
        len == sizeof(saved) && saved.valid) {
        if (s_lock != NULL) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        s_snap = saved;
        if (s_lock != NULL) {
            xSemaphoreGive(s_lock);
        }
        ESP_LOGI(TAG, "restored last hitokoto from nvs: #%ld %s", (long)saved.id, saved.text);
    }
    nvs_close(handle);
}

/* ------------------------------------------------------------------ */
/* 拉取与解析                                                           */
/* ------------------------------------------------------------------ */

/** UTF-8 边界安全截断（与 UI 侧同规则：不切断多字节序列）。 */
static void hitokoto_str_copy(char *dst, size_t n, const cJSON *item) {
    dst[0] = '\0';
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return;
    }
    const char *src = item->valuestring;
    size_t len = strlen(src);
    if (len >= n) {
        /* 回退到 UTF-8 字符边界：跳过续字节（10xxxxxx）。 */
        len = n - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/** 拉取一条一言并更新快照。@return ESP_OK=成功（含业务字段校验通过）。 */
static esp_err_t hitokoto_fetch_once(void) {
    char url[192];
    snprintf(url, sizeof(url), HITOKOTO_API_URL "?max_length=%d", HITOKOTO_MAX_LENGTH);
    for (const char *c = HITOKOTO_CATEGORIES; *c != '\0'; c++) {
        const size_t len = strlen(url);
        snprintf(url + len, sizeof(url) - len, "&c=%c", *c);
    }

    const nethttp_cfg_t cfg = {
        .url = url,
        .max_len = HITOKOTO_RESP_MAX,
        .prealloc = false,
        .max_retries = HITOKOTO_HTTP_MAX_RETRIES,
        /* 站点专用 CA：该站链顶是 GlobalSign R1 交叉签名的 GTS Root R4，
         * R1 不在 IDF 证书包内，全局包校验必败（见证书头文件说明）。 */
        .cert_pem = HITOKOTO_CERT_PEM,
    };
    char *body = NULL;
    const esp_err_t err = nethttp_get(&cfg, &body, NULL);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "invalid json response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "hitokoto");
    if (!cJSON_IsString(text) || text->valuestring == NULL || text->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "response missing hitokoto field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 组装好后一次性换入快照（读到的是完整一致的一条）。 */
    espaperplay_hitokoto_t next = {0};
    next.valid = true;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    next.id = cJSON_IsNumber(id) ? (int32_t)id->valuedouble : 0;
    hitokoto_str_copy(next.text, sizeof(next.text), text);
    hitokoto_str_copy(next.from, sizeof(next.from), cJSON_GetObjectItemCaseSensitive(root, "from"));
    hitokoto_str_copy(next.from_who, sizeof(next.from_who),
                      cJSON_GetObjectItemCaseSensitive(root, "from_who"));
    hitokoto_str_copy(next.type, sizeof(next.type), cJSON_GetObjectItemCaseSensitive(root, "type"));

    cJSON_Delete(root);

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_snap = next;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    hitokoto_persist(&next);
    ESP_LOGI(TAG, "hitokoto #%ld: %s (%s%s%s)", (long)next.id, next.text, next.from,
             next.from_who[0] ? " · " : "", next.from_who);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 后台任务                                                             */
/* ------------------------------------------------------------------ */

/** 后台刷新任务：等待联网 -> 拉取 -> 按周期刷新；可被 xTaskNotify 唤醒
 * 立即刷新（"换一句"）。成功后睡整周期，失败后睡短周期重试。 */
static void hitokoto_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "hitokoto task started");

    int waited = 0;
    while (waited < HITOKOTO_WIFI_WAIT_MS && !espaperplay_wifi_is_sta_online()) {
        vTaskDelay(pdMS_TO_TICKS(HITOKOTO_WIFI_POLL_MS));
        waited += HITOKOTO_WIFI_POLL_MS;
    }
    if (!espaperplay_wifi_is_sta_online()) {
        ESP_LOGW(TAG, "no STA network at startup, hitokoto will be fetched once online");
    }

    while (1) {
        /* 失败退避：无效态（尚未取到第一条）短周期重试——设备睡眠期间
         * WiFi 断开、拉取必然失败，此间隔只在清醒时起效，缩短拿到第一
         * 条的等待；已有内容则失败按短退避、成功按整周期。 */
        espaperplay_hitokoto_t cur;
        espaperplay_hitokoto_get(&cur);
        uint32_t next_wait = cur.valid ? s_interval_ms : HITOKOTO_INVALID_RETRY_MS;
        if (espaperplay_wifi_is_sta_online()) {
            const esp_err_t err = hitokoto_fetch_once();
            if (err != ESP_OK) {
                next_wait = cur.valid ? ESPAPERPLAY_HITOKOTO_RETRY_INTERVAL_MS
                                      : HITOKOTO_INVALID_RETRY_MS;
                ESP_LOGW(TAG, "hitokoto fetch failed: %s (retry in %u ms)", esp_err_to_name(err),
                         (unsigned)next_wait);
            }
            /* 监控任务栈余量（TLS 握手等路径的栈占用），便于发现潜在溢出。 */
            ESP_LOGD(TAG, "task stack high water: %u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else {
            ESP_LOGD(TAG, "no STA network, skip hitokoto fetch");
        }
        /* 等待周期或立即刷新通知（通知返回 pdTRUE，立即进入下一轮）。 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(next_wait));
    }
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_hitokoto_start(void) {
    if (s_lock == NULL) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (m == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_lock = m;
    }
    hitokoto_restore(); /* 先恢复上次内容：开机即有可用句子 */
    if (s_task != NULL) {
        return ESP_OK; /* 幂等 */
    }
    if (xTaskCreate(hitokoto_task, "hitokoto_task", HITOKOTO_TASK_STACK_SIZE, NULL,
                    HITOKOTO_TASK_PRIORITY, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create hitokoto task");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "hitokoto task created");
    return ESP_OK;
}

esp_err_t espaperplay_hitokoto_get(espaperplay_hitokoto_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    *out = s_snap;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

void espaperplay_hitokoto_request_refresh(void) {
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    TaskHandle_t task = s_task;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (task != NULL) {
        xTaskNotifyGive(task);
        ESP_LOGI(TAG, "refresh requested");
    } else {
        ESP_LOGW(TAG, "refresh requested but task not running");
    }
}

const char *espaperplay_hitokoto_type_name(const char *type) {
    if (type == NULL || type[0] == '\0' || type[1] != '\0') {
        return NULL;
    }
    switch (type[0]) {
        case 'a':
            return "动画";
        case 'b':
            return "漫画";
        case 'c':
            return "游戏";
        case 'd':
            return "文学";
        case 'e':
            return "原创";
        case 'f':
            return "网络";
        case 'g':
            return "其他";
        case 'h':
            return "影视";
        case 'i':
            return "诗词";
        case 'j':
            return "网易云";
        case 'k':
            return "哲学";
        case 'l':
            return "抖机灵";
        default:
            return NULL;
    }
}
