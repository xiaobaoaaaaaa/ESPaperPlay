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
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "cJSON.h"
#include "zlib.h"

#include "espaperplay_geoip.h"
#include "espaperplay_netip.h"
#include "espaperplay_system.h"
#include "espaperplay_weather.h"
#include "espaperplay_wifi.h"

static const char *TAG = "ESPaperPlay_WEATHER";

/** 天气数据 API 主机（默认公共地址：开发版 / 免费订阅）。 */
#define ESPAPERPLAY_WEATHER_API_HOST "https://devapi.qweather.com"
/** GeoAPI（城市查询 / 经纬度反查）主机（默认公共地址）。 */
#define ESPAPERPLAY_WEATHER_GEO_HOST "https://geoapi.qweather.com"
/** GeoAPI 请求路径：使用默认公共地址时为旧路径，配置自定义 API Host 时为新路径。 */
#define ESPAPERPLAY_WEATHER_GEO_PATH_LEGACY "/v2/city/lookup"
#define ESPAPERPLAY_WEATHER_GEO_PATH_CUSTOM "/geo/v2/city/lookup"

/**
 * @brief 选择 API 主机。
 *
 * 和风天气自 2026 年起逐步停止公共地址（devapi/geoapi.qweather.com），
 * 每个开发者帐号拥有独立 API Host（控制台-设置 查看，形如
 * "abc1234xyz.def.qweatherapi.com"，不含协议前缀）。配置了自定义 API Host
 * 时，天气数据与 GeoAPI 均使用该主机（GeoAPI 路径切换为新版 /geo/v2/...）；
 * 未配置时回退到默认公共地址。
 */

/** 自定义 API Host 合成缓冲（"https://" + 最长 80 字符 Host + NUL）。 */
#define WEATHER_CUSTOM_HOST_BUF_SIZE (ESPAPERPLAY_SYSTEM_WEATHER_HOST_MAX_LEN + 16)
static char s_custom_host[WEATHER_CUSTOM_HOST_BUF_SIZE];

/**
 * @brief 规范化自定义 API Host 并补全协议前缀（线程安全：仅在请求前调用）。
 *
 * 控制台给出的 API Host 不含协议（"abc1234xyz.def.qweatherapi.com"）；
 * 兼容用户手输 "https://" 前缀与末尾 "/" 的情况。
 */
static const char *weather_custom_host(void) {
    const char *custom = espaperplay_system_get_config()->weather_api_host;
    if (custom[0] == '\0') {
        return NULL;
    }
    const char *host = custom;
    if (strncmp(host, "https://", 8) == 0) {
        host += 8;
    } else if (strncmp(host, "http://", 7) == 0) {
        host += 7;
    }
    size_t len = strlen(host);
    while (len > 0 && (host[len - 1] == '/' || host[len - 1] == ' ')) {
        len--;
    }
    snprintf(s_custom_host, sizeof(s_custom_host), "https://%.*s", (int)len, host);
    return s_custom_host;
}

static void weather_get_hosts(const char **data_host, const char **geo_host,
                              const char **geo_path) {
    const char *custom = weather_custom_host();
    if (custom != NULL) {
        *data_host = custom;
        *geo_host = custom;
        *geo_path = ESPAPERPLAY_WEATHER_GEO_PATH_CUSTOM;
    } else {
        *data_host = ESPAPERPLAY_WEATHER_API_HOST;
        *geo_host = ESPAPERPLAY_WEATHER_GEO_HOST;
        *geo_path = ESPAPERPLAY_WEATHER_GEO_PATH_LEGACY;
    }
}

/** 天气数据 API 主机（配置了自定义 API Host 时使用之，否则默认公共地址）。 */
static const char *weather_data_host(void) {
    const char *custom = weather_custom_host();
    return custom != NULL ? custom : ESPAPERPLAY_WEATHER_API_HOST;
}

/** 各接口响应体最大字节数（超过截断并视为异常）。 */
#define ESPAPERPLAY_WEATHER_RESP_NOW_MAX 4096
#define ESPAPERPLAY_WEATHER_RESP_DAILY_MAX 16384
#define ESPAPERPLAY_WEATHER_RESP_HOURLY_MAX 16384
#define ESPAPERPLAY_WEATHER_RESP_MINUTELY_MAX 8192
#define ESPAPERPLAY_WEATHER_RESP_WARNING_MAX 16384
#define ESPAPERPLAY_WEATHER_RESP_INDICES_MAX 16384
#define ESPAPERPLAY_WEATHER_RESP_AIR_MAX 4096
#define ESPAPERPLAY_WEATHER_RESP_ASTRONOMY_MAX 4096
#define ESPAPERPLAY_WEATHER_RESP_LOOKUP_MAX 16384

/** 城市查询缓存条目数（按查询串区分，满时淘汰最旧）。 */
#define ESPAPERPLAY_WEATHER_LOOKUP_CACHE_ENTRIES 4
/** 城市查询结果缓存有效期（毫秒）：24 小时。 */
#define ESPAPERPLAY_WEATHER_LOOKUP_TTL_MS (24 * 60 * 60 * 1000)

/** 后台任务栈大小（含 HTTPS/TLS 握手开销；天气数据结构体较大，均在堆上分配）。 */
#define ESPAPERPLAY_WEATHER_TASK_STACK_SIZE 16384
/** 后台任务优先级。 */
#define ESPAPERPLAY_WEATHER_TASK_PRIORITY 3
/** 后台任务启动时等待 STA 联网的最长时间（毫秒）。 */
#define ESPAPERPLAY_WEATHER_WIFI_WAIT_MS 60000
/** WiFi 状态轮询间隔（毫秒）。 */
#define ESPAPERPLAY_WEATHER_WIFI_POLL_MS 1000

/** 快照中各类数据数组容量（与 API 返回上限一致）。 */
#define WEATHER_DAILY_MAX 7
#define WEATHER_HOURLY_MAX 24
#define WEATHER_WARNING_MAX 8
#define WEATHER_INDICES_MAX 16
#define WEATHER_MINUTELY_MAX 120
#define WEATHER_LOOKUP_RESULT_MAX 4

/* ------------------------------------------------------------------ */
/* 全局状态                                                             */
/* ------------------------------------------------------------------ */

static SemaphoreHandle_t s_lock = NULL; /*!< 缓存 / 快照 / 状态访问互斥锁 */
static TaskHandle_t s_task = NULL;      /*!< 后台刷新任务句柄（NULL=未运行） */
static volatile uint32_t s_refresh_interval_ms = ESPAPERPLAY_WEATHER_REFRESH_INTERVAL_MS;

/*!< 上次成功整体刷新的时刻（esp_timer，毫秒；0=从未成功）。用于电源管理
 * 判定睡眠期间天气是否过期（到期则借定时器唤醒重连拉取）。 */
static volatile int64_t s_last_refresh_ok_ms = 0;
/*!< 上次真实发起整体刷新的时刻（含失败；失败退避用，避免持续失败时
 * 每次分钟唤醒都白白重连 WiFi）。 */
static volatile int64_t s_last_refresh_try_ms = 0;
/*!< 刷新完成信号（每次刷新尝试结束置位，供外部等待完成）。 */
static EventGroupHandle_t s_refresh_done = NULL;

/*!< 数据快照（PSRAM 惰性分配，见 weather_caches_ensure；NULL=尚未就绪）。 */
static espaperplay_weather_snapshot_t *s_snapshot;
/*!< 运行状态（含最近错误；PSRAM 惰性分配）。 */
static espaperplay_weather_status_t *s_status;

/* 各 API 独立缓存（键 = 实际使用的 LocationID）。 */
typedef struct {
    bool valid;    /*!< 是否已有有效缓存 */
    uint64_t ts_ms; /*!< 缓存写入时间（esp_timer，毫秒） */
    char loc[16];  /*!< 缓存对应的 LocationID */
    espaperplay_weather_now_t data;
} weather_now_cache_t;

typedef struct {
    bool valid;
    uint64_t ts_ms;
    char loc[16];
    espaperplay_weather_daily_t data[WEATHER_DAILY_MAX];
    int count;
} weather_daily_cache_t;

typedef struct {
    bool valid;
    uint64_t ts_ms;
    char loc[16];
    espaperplay_weather_hourly_t data[WEATHER_HOURLY_MAX];
    int count;
} weather_hourly_cache_t;

typedef struct {
    bool valid;
    uint64_t ts_ms;
    char loc[16];
    espaperplay_weather_minutely_t data;
} weather_minutely_cache_t;

typedef struct {
    bool valid;
    uint64_t ts_ms;
    char loc[16];
    espaperplay_weather_warning_t data[WEATHER_WARNING_MAX];
    int count;
} weather_warning_cache_t;

typedef struct {
    bool valid;
    uint64_t ts_ms;
    char loc[16];
    espaperplay_weather_indices_t data[WEATHER_INDICES_MAX];
    int count;
} weather_indices_cache_t;

typedef struct {
    bool valid;
    uint64_t ts_ms;
    char loc[16];
    espaperplay_weather_air_t data;
} weather_air_cache_t;

typedef struct {
    bool valid;
    uint64_t ts_ms;
    char loc[16];
    espaperplay_weather_astronomy_t data;
    /*! 最近一次非空月出（含日期隐含：仅当日月出非空时更新；今日无月出时它
     * 必为前一日值——月升每 24.8h 一次，日历日最多缺一天）。和风 astronomy
     * date 参数仅支持今日起未来 60 天（过去日期 400），前一日月出只能靠缓存。 */
    char last_moonrise[24];
} weather_astronomy_cache_t;

/* 各接口缓存合计约 30KB、快照约 22KB：均为纯 CPU 访问、不参与 DMA，
 * 全部放 PSRAM（指针惰性分配，见 weather_caches_ensure），为内部 RAM 让路。 */
static weather_now_cache_t *s_cache_now;
static weather_daily_cache_t *s_cache_daily_3d;
static weather_daily_cache_t *s_cache_daily_7d;
static weather_hourly_cache_t *s_cache_hourly;
static weather_minutely_cache_t *s_cache_minutely;
static weather_warning_cache_t *s_cache_warning;
static weather_indices_cache_t *s_cache_indices;
static weather_air_cache_t *s_cache_air;
static weather_astronomy_cache_t *s_cache_astronomy;

/* 城市查询缓存（按查询串区分）。 */
typedef struct {
    bool valid;
    uint64_t ts_ms;
    char query[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN];
    int count;
    espaperplay_weather_location_t loc[WEATHER_LOOKUP_RESULT_MAX];
} weather_lookup_cache_entry_t;

static weather_lookup_cache_entry_t *s_cache_lookup; /*!< [ESPAPERPLAY_WEATHER_LOOKUP_CACHE_ENTRIES] */

/* 自动定位结果缓存。 */
typedef struct {
    bool valid;
    uint64_t ts_ms;
    bool auto_location;
    char id[16];
    char name[128];
} weather_auto_loc_cache_t;

static weather_auto_loc_cache_t *s_cache_auto_loc;

/* ------------------------------------------------------------------ */
/* 锁与时间辅助                                                         */
/* ------------------------------------------------------------------ */

/** 大块缓存并发分配保护（分配在临界区外做，临界区内只登记指针）。 */
static portMUX_TYPE s_cache_alloc_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 惰性分配一块缓存（PSRAM 优先，PSRAM 不足回落内部 RAM）。
 *
 * @param slot 指针槽（成功后写入缓存地址）。
 * @param size 缓存字节数。
 * @return 缓存指针；两类内存均不足时返回 NULL（调用方按缓存未命中降级）。
 */
static void *weather_block_ensure(void **slot, size_t size) {
    if (*slot != NULL) {
        return *slot;
    }
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
    }
    if (p == NULL) {
        return NULL;
    }
    portENTER_CRITICAL(&s_cache_alloc_mux);
    if (*slot == NULL) {
        *slot = p;
        p = NULL;
    }
    portEXIT_CRITICAL(&s_cache_alloc_mux);
    if (p != NULL) {
        /* 并发下另一任务已抢先登记：释放本份，复用已登记块。 */
        free(p);
    }
    return *slot;
}

/** 确保全部大块缓存已分配（快照 / 状态 / 各接口缓存 / 城市查询缓存）。 */
static void weather_caches_ensure(void) {
    weather_block_ensure((void **)&s_snapshot, sizeof(*s_snapshot));
    weather_block_ensure((void **)&s_status, sizeof(*s_status));
    weather_block_ensure((void **)&s_cache_now, sizeof(*s_cache_now));
    weather_block_ensure((void **)&s_cache_daily_3d, sizeof(*s_cache_daily_3d));
    weather_block_ensure((void **)&s_cache_daily_7d, sizeof(*s_cache_daily_7d));
    weather_block_ensure((void **)&s_cache_hourly, sizeof(*s_cache_hourly));
    weather_block_ensure((void **)&s_cache_minutely, sizeof(*s_cache_minutely));
    weather_block_ensure((void **)&s_cache_warning, sizeof(*s_cache_warning));
    weather_block_ensure((void **)&s_cache_indices, sizeof(*s_cache_indices));
    weather_block_ensure((void **)&s_cache_air, sizeof(*s_cache_air));
    weather_block_ensure((void **)&s_cache_astronomy, sizeof(*s_cache_astronomy));
    weather_block_ensure((void **)&s_cache_lookup,
                         sizeof(*s_cache_lookup) * ESPAPERPLAY_WEATHER_LOOKUP_CACHE_ENTRIES);
    weather_block_ensure((void **)&s_cache_auto_loc, sizeof(*s_cache_auto_loc));
}

/** 确保互斥锁与 PSRAM 大块缓存已就绪（惰性初始化，可重入）。 */
static void weather_lock_ensure(void) {
    if (s_lock == NULL) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (m != NULL) {
            s_lock = m;
        }
    }
    weather_caches_ensure();
}

/** 当前时间（毫秒，esp_timer）。 */
static uint64_t weather_now_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ */
/* HTTP 层                                                              */
/* ------------------------------------------------------------------ */

/** 响应累积缓冲（data 由调用方按 max 一次性分配，避免 realloc 增长
 *  在内部 RAM 制造碎片；>=8KB 的大缓冲随 SPIRAM_MALLOC_ALWAYSINTERNAL
 *  阈值自动落入 PSRAM）。 */
typedef struct {
    char *data; /*!< 响应体缓冲（NUL 结尾） */
    size_t len; /*!< 已接收字节数（不含结尾 NUL） */
    size_t max; /*!< 缓冲容量（= 允许的最大字节数） */
} weather_resp_t;

/** esp_http_client 事件回调：把响应体分块累积进预分配的缓冲。 */
static esp_err_t weather_http_event_handler(esp_http_client_event_t *evt) {
    weather_resp_t *resp = (weather_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t need = resp->len + evt->data_len + 1;
        if (need > resp->max) {
            ESP_LOGW(TAG, "response too large (%u bytes), aborting", (unsigned)need);
            return ESP_FAIL;
        }
        memcpy(resp->data + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->data[resp->len] = '\0';
    }
    return ESP_OK;
}

/** gzip 魔数（RFC 1952：0x1f 0x8b）。 */
#define WEATHER_GZIP_MAGIC_1 0x1f
#define WEATHER_GZIP_MAGIC_2 0x8b

/**
 * @brief 解压 gzip / zlib 数据。
 *
 * 和风天气 API 默认使用 gzip 压缩响应（官方文档"处理Gzip"），而
 * esp_http_client 不会自动解压；检测到 gzip 魔数时用 zlib 解压。
 * 使用 windowBits = 15+32 自动识别 gzip 与 zlib 两种封装。
 *
 * @param src      压缩数据。
 * @param src_len  压缩数据长度。
 * @param max_out  明文输出上限（超过视为异常，返回 NULL）。
 * @param out_len  输出明文长度。
 * @return malloc 的明文缓冲（NUL 结尾，调用方负责 free）；失败返回 NULL。
 */
static char *weather_inflate(const char *src, size_t src_len, size_t max_out, size_t *out_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 15 + 32) != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed");
        return NULL;
    }
    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;

    size_t cap = src_len * 4 + 512; /* gzip JSON 通常膨胀 3-10 倍，先按 4 倍起步 */
    if (cap < 1024) {
        cap = 1024;
    }
    if (cap > max_out) {
        cap = max_out;
    }
    /* 多分配 1 字节：输出恰好填满缓冲时仍需结尾 NUL（见函数末尾）。 */
    char *out = malloc(cap + 1);
    if (out == NULL) {
        inflateEnd(&strm);
        return NULL;
    }
    strm.next_out = (Bytef *)out;
    strm.avail_out = (uInt)cap;

    int ret;
    do {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_OK && strm.avail_out == 0) {
            if (strm.total_out >= max_out) {
                ret = Z_BUF_ERROR;
                break;
            }
            size_t new_cap = cap * 2;
            if (new_cap > max_out) {
                new_cap = max_out;
            }
            char *n = realloc(out, new_cap + 1);
            if (n == NULL) {
                ret = Z_MEM_ERROR;
                break;
            }
            out = n;
            strm.next_out = (Bytef *)(out + strm.total_out);
            strm.avail_out = (uInt)(new_cap - strm.total_out);
            cap = new_cap;
        }
    } while (ret == Z_OK);

    inflateEnd(&strm);
    if (ret != Z_STREAM_END) {
        ESP_LOGW(TAG, "inflate failed (ret=%d, in=%u bytes)", ret, (unsigned)src_len);
        free(out);
        return NULL;
    }
    out[strm.total_out] = '\0';
    *out_len = strm.total_out;
    return out;
}

/** 打印响应体前若干字节（非可打印字符以 '.' 代替），用于诊断无法识别的响应。 */
static void weather_log_body_preview(const char *body, size_t len) {
    char buf[128];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < sizeof(buf); i++) {
        unsigned char c = (unsigned char)body[i];
        buf[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    buf[o] = '\0';
    ESP_LOGW(TAG, "response body preview (%u bytes): \"%s\"", (unsigned)len, buf);
}

/** 瞬时连接失败的最大重试次数（网络抖动 / LWIP 连接池暂满时自愈重试）。 */
#define WEATHER_HTTP_MAX_RETRIES 2

/**
 * @brief 发起一次携带 API Key 的 HTTPS GET 请求并返回完整响应体。
 *
 * 使用 ESP-IDF 内置 CA 证书包（esp_crt_bundle）校验服务器证书；API Key
 * 通过 X-QW-Api-Key 请求头传递。瞬时连接失败（connect 被拒 / 连接被对端
 * 关闭 / 超时）自动重试 WEATHER_HTTP_MAX_RETRIES 次（退避 300/600ms），
 * 用于自愈 LWIP 连接池暂满（TIME_WAIT 堆积）等场景。
 *
 * @param url     请求地址（非空）。
 * @param api_key API Key（非空）。
 * @param max_len 响应体上限（字节）。
 * @param out_body 成功时输出 malloc 的响应体字符串（调用方负责 free）。
 *
 * @return ESP_OK 且 HTTP 状态为 200 时成功，否则返回错误码。
 */
static esp_err_t weather_http_get(const char *url, const char *api_key, size_t max_len,
                                  char **out_body) {
    for (int attempt = 0;; attempt++) {
        /* 响应缓冲按上限一次性分配（大缓冲落 PSRAM，不做 realloc 增长）。 */
        weather_resp_t resp = {0};
        resp.max = max_len;
        resp.data = malloc(max_len + 1);
        if (resp.data == NULL) {
            ESP_LOGE(TAG, "failed to allocate response buffer (%u bytes)", (unsigned)max_len);
            return ESP_ERR_NO_MEM;
        }
        resp.data[0] = '\0';

        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = ESPAPERPLAY_WEATHER_HTTP_TIMEOUT_MS,
            .disable_auto_redirect = true,
            .event_handler = weather_http_event_handler,
            .user_data = &resp,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == NULL) {
            ESP_LOGE(TAG, "failed to init http client");
            free(resp.data);
            return ESP_ERR_NO_MEM;
        }
        esp_http_client_set_header(client, "X-QW-Api-Key", api_key);
        /* 明确要求不压缩：esp_http_client 不解压 gzip，防止服务端按
         * Accept-Encoding 返回压缩体导致 JSON 解析失败。 */
        esp_http_client_set_header(client, "Accept-Encoding", "identity");

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        /* 瞬时连接失败：重试（最多 WEATHER_HTTP_MAX_RETRIES 次，指数退避）。 */
        const bool transient = (err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_HTTP_CONNECTION_CLOSED ||
                                err == ESP_ERR_TIMEOUT);
        if (transient && attempt < WEATHER_HTTP_MAX_RETRIES) {
            ESP_LOGW(TAG, "http request failed (%s), retrying %d/%d",
                     esp_err_to_name(err), attempt + 1, WEATHER_HTTP_MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(300u << attempt));
            free(resp.data);
            continue;
        }

        if (err != ESP_OK) {
            /* 输出内存诊断：区分内部 RAM 不足（TLS 缓冲分配失败）与网络问题。 */
            ESP_LOGE(TAG,
                     "http request failed: %s (heap internal free=%u largest=%u, total free=%u)",
                     esp_err_to_name(err), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)esp_get_free_heap_size());
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

        /* 和风天气 API 默认 gzip 压缩：检测魔数并解压（esp_http_client 不自动解压）。 */
        if (resp.len >= 2 && (uint8_t)resp.data[0] == WEATHER_GZIP_MAGIC_1 &&
            (uint8_t)resp.data[1] == WEATHER_GZIP_MAGIC_2) {
            ESP_LOGD(TAG, "response is gzip (%u bytes), inflating", (unsigned)resp.len);
            size_t plain_len = 0;
            char *plain = weather_inflate(resp.data, resp.len, max_len, &plain_len);
            free(resp.data);
            if (plain == NULL) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            resp.data = plain;
            resp.len = plain_len;
        }

        *out_body = resp.data;
        return ESP_OK;
    }
}

/* ------------------------------------------------------------------ */
/* 状态记录                                                             */
/* ------------------------------------------------------------------ */

/** QWeather 业务码含义（https://dev.qweather.com/docs/resource/status-code/）。 */
static const char *weather_code_meaning(int code) {
    switch (code) {
    case 200:
        return "请求成功";
    case 204:
        return "无内容（该地区不支持此数据）";
    case 400:
        return "请求参数错误";
    case 401:
        return "无效的 API Key";
    case 402:
        return "API Key 无权限（订阅不包含该接口）";
    case 403:
        return "API Key 请求超出配额";
    case 404:
        return "无数据（LocationID 无效等）";
    case 429:
        return "请求过于频繁";
    case 500:
        return "服务器内部错误";
    default:
        return "未知错误";
    }
}

/** 记录最近一次错误（线程安全）。 */
static void weather_record_error(int api_code, const char *msg) {
    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_status != NULL) {
        s_status->last_api_code = api_code;
        strlcpy(s_status->last_error, msg, sizeof(s_status->last_error));
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/** 记录一次 API 调用失败：组合「接口名 + 业务码 + 含义」。 */
static void weather_record_api_error(const char *api_name, int code) {
    char msg[sizeof(s_status->last_error)];
    snprintf(msg, sizeof(msg), "%s: QWeather code %d（%s）", api_name, code,
             weather_code_meaning(code));
    ESP_LOGW(TAG, "%s", msg);
    weather_record_error(code, msg);
}

/* ------------------------------------------------------------------ */
/* JSON 解析辅助                                                        */
/* ------------------------------------------------------------------ */

/** 按点分路径（如 "now.temp"）在 JSON 树中查找节点。 */
static const cJSON *weather_json_find(const cJSON *root, const char *path) {
    const char *dot = strchr(path, '.');
    if (dot == NULL) {
        return cJSON_GetObjectItemCaseSensitive(root, path);
    }
    if ((size_t)(dot - path) >= 32) {
        return NULL;
    }
    char key[32];
    memcpy(key, path, (size_t)(dot - path));
    key[dot - path] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(item)) {
        return NULL;
    }
    return weather_json_find(item, dot + 1);
}

/** 从 JSON 树复制字符串字段到目标缓冲（字段缺失时保持原值）。 */
static void weather_copy_field(const cJSON *root, const char *path, char *dst, size_t dst_len) {
    const cJSON *item = weather_json_find(root, path);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(dst, item->valuestring, dst_len);
    }
}

/** 解析响应体中的业务码（"code" 字段，字符串形式）。返回 0 表示解析失败。 */
static int weather_parse_code(const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return 0;
    }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    int value = 0;
    if (cJSON_IsNumber(code)) {
        value = (int)code->valuedouble;
    } else if (cJSON_IsString(code) && code->valuestring != NULL) {
        value = atoi(code->valuestring);
    }
    cJSON_Delete(root);
    return value;
}

/* ------------------------------------------------------------------ */
/* 各接口解析                                                           */
/* ------------------------------------------------------------------ */

static esp_err_t weather_parse_now(const char *body, espaperplay_weather_now_t *out) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    weather_copy_field(root, "now.obsTime", out->obs_time, sizeof(out->obs_time));
    weather_copy_field(root, "now.temp", out->temp, sizeof(out->temp));
    weather_copy_field(root, "now.feelsLike", out->feels_like, sizeof(out->feels_like));
    weather_copy_field(root, "now.icon", out->icon, sizeof(out->icon));
    weather_copy_field(root, "now.text", out->text, sizeof(out->text));
    weather_copy_field(root, "now.windDir", out->wind_dir, sizeof(out->wind_dir));
    weather_copy_field(root, "now.windScale", out->wind_scale, sizeof(out->wind_scale));
    weather_copy_field(root, "now.windSpeed", out->wind_speed, sizeof(out->wind_speed));
    weather_copy_field(root, "now.humidity", out->humidity, sizeof(out->humidity));
    weather_copy_field(root, "now.precip", out->precip, sizeof(out->precip));
    weather_copy_field(root, "now.pressure", out->pressure, sizeof(out->pressure));
    weather_copy_field(root, "now.vis", out->vis, sizeof(out->vis));
    weather_copy_field(root, "now.cloud", out->cloud, sizeof(out->cloud));
    weather_copy_field(root, "now.dew", out->dew, sizeof(out->dew));
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t weather_parse_daily(const char *body, espaperplay_weather_daily_t *out,
                                     int *out_count) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "daily");
    int count = cJSON_GetArraySize(arr);
    if (count > WEATHER_DAILY_MAX) {
        count = WEATHER_DAILY_MAX;
    }
    for (int i = 0; i < count; i++) {
        const cJSON *d = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(d)) {
            continue;
        }
        espaperplay_weather_daily_t *dst = &out[i];
        weather_copy_field(d, "fxDate", dst->fx_date, sizeof(dst->fx_date));
        weather_copy_field(d, "sunrise", dst->sunrise, sizeof(dst->sunrise));
        weather_copy_field(d, "sunset", dst->sunset, sizeof(dst->sunset));
        weather_copy_field(d, "moonrise", dst->moonrise, sizeof(dst->moonrise));
        weather_copy_field(d, "moonset", dst->moonset, sizeof(dst->moonset));
        weather_copy_field(d, "moonPhase", dst->moon_phase, sizeof(dst->moon_phase));
        weather_copy_field(d, "tempMax", dst->temp_max, sizeof(dst->temp_max));
        weather_copy_field(d, "tempMin", dst->temp_min, sizeof(dst->temp_min));
        weather_copy_field(d, "iconDay", dst->icon_day, sizeof(dst->icon_day));
        weather_copy_field(d, "textDay", dst->text_day, sizeof(dst->text_day));
        weather_copy_field(d, "iconNight", dst->icon_night, sizeof(dst->icon_night));
        weather_copy_field(d, "textNight", dst->text_night, sizeof(dst->text_night));
        weather_copy_field(d, "windDirDay", dst->wind_dir_day, sizeof(dst->wind_dir_day));
        weather_copy_field(d, "windScaleDay", dst->wind_scale_day, sizeof(dst->wind_scale_day));
        weather_copy_field(d, "windSpeedDay", dst->wind_speed_day, sizeof(dst->wind_speed_day));
        weather_copy_field(d, "windDirNight", dst->wind_dir_night, sizeof(dst->wind_dir_night));
        weather_copy_field(d, "windScaleNight", dst->wind_scale_night,
                           sizeof(dst->wind_scale_night));
        weather_copy_field(d, "windSpeedNight", dst->wind_speed_night,
                           sizeof(dst->wind_speed_night));
        weather_copy_field(d, "humidity", dst->humidity, sizeof(dst->humidity));
        weather_copy_field(d, "precip", dst->precip, sizeof(dst->precip));
        weather_copy_field(d, "pressure", dst->pressure, sizeof(dst->pressure));
        weather_copy_field(d, "vis", dst->vis, sizeof(dst->vis));
        weather_copy_field(d, "cloud", dst->cloud, sizeof(dst->cloud));
        weather_copy_field(d, "uvIndex", dst->uv_index, sizeof(dst->uv_index));
    }
    cJSON_Delete(root);
    *out_count = count;
    return ESP_OK;
}

static esp_err_t weather_parse_hourly(const char *body, espaperplay_weather_hourly_t *out,
                                      int *out_count) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    int count = cJSON_GetArraySize(arr);
    if (count > WEATHER_HOURLY_MAX) {
        count = WEATHER_HOURLY_MAX;
    }
    for (int i = 0; i < count; i++) {
        const cJSON *h = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(h)) {
            continue;
        }
        espaperplay_weather_hourly_t *dst = &out[i];
        weather_copy_field(h, "fxTime", dst->fx_time, sizeof(dst->fx_time));
        weather_copy_field(h, "temp", dst->temp, sizeof(dst->temp));
        weather_copy_field(h, "icon", dst->icon, sizeof(dst->icon));
        weather_copy_field(h, "text", dst->text, sizeof(dst->text));
        weather_copy_field(h, "windDir", dst->wind_dir, sizeof(dst->wind_dir));
        weather_copy_field(h, "windScale", dst->wind_scale, sizeof(dst->wind_scale));
        weather_copy_field(h, "windSpeed", dst->wind_speed, sizeof(dst->wind_speed));
        weather_copy_field(h, "humidity", dst->humidity, sizeof(dst->humidity));
        weather_copy_field(h, "precip", dst->precip, sizeof(dst->precip));
        weather_copy_field(h, "pressure", dst->pressure, sizeof(dst->pressure));
        weather_copy_field(h, "cloud", dst->cloud, sizeof(dst->cloud));
        weather_copy_field(h, "dew", dst->dew, sizeof(dst->dew));
    }
    cJSON_Delete(root);
    *out_count = count;
    return ESP_OK;
}

static esp_err_t weather_parse_minutely(const char *body, espaperplay_weather_minutely_t *out) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    weather_copy_field(root, "summary", out->summary, sizeof(out->summary));
    weather_copy_field(root, "fxLink", out->fx_link, sizeof(out->fx_link));
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "minutely");
    int count = cJSON_GetArraySize(arr);
    if (count > WEATHER_MINUTELY_MAX) {
        count = WEATHER_MINUTELY_MAX;
    }
    out->count = 0;
    for (int i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            /* 旧格式：字符串数组 ["0.00","0.00",...]。 */
            strlcpy(out->precip[out->count], item->valuestring, sizeof(out->precip[out->count]));
            out->count++;
        } else if (cJSON_IsObject(item)) {
            /* 新格式：对象数组 [{"fxTime":...,"precip":"0.15","type":"rain"},...]。 */
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(item, "precip");
            if (cJSON_IsString(p) && p->valuestring != NULL) {
                strlcpy(out->precip[out->count], p->valuestring,
                        sizeof(out->precip[out->count]));
                out->count++;
            }
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t weather_parse_warning(const char *body, espaperplay_weather_warning_t *out,
                                       int *out_count) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "warning");
    int count = cJSON_GetArraySize(arr);
    if (count > WEATHER_WARNING_MAX) {
        count = WEATHER_WARNING_MAX;
    }
    for (int i = 0; i < count; i++) {
        const cJSON *w = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(w)) {
            continue;
        }
        espaperplay_weather_warning_t *dst = &out[i];
        weather_copy_field(w, "id", dst->id, sizeof(dst->id));
        weather_copy_field(w, "sender", dst->sender, sizeof(dst->sender));
        weather_copy_field(w, "pubTime", dst->pub_time, sizeof(dst->pub_time));
        weather_copy_field(w, "title", dst->title, sizeof(dst->title));
        weather_copy_field(w, "startTime", dst->start_time, sizeof(dst->start_time));
        weather_copy_field(w, "endTime", dst->end_time, sizeof(dst->end_time));
        weather_copy_field(w, "type", dst->type, sizeof(dst->type));
        weather_copy_field(w, "typeName", dst->type_name, sizeof(dst->type_name));
        weather_copy_field(w, "level", dst->level, sizeof(dst->level));
        weather_copy_field(w, "severity", dst->severity, sizeof(dst->severity));
        weather_copy_field(w, "severityColor", dst->severity_color, sizeof(dst->severity_color));
        weather_copy_field(w, "text", dst->text, sizeof(dst->text));
        weather_copy_field(w, "related", dst->related, sizeof(dst->related));
    }
    cJSON_Delete(root);
    *out_count = count;
    return ESP_OK;
}

static esp_err_t weather_parse_indices(const char *body, espaperplay_weather_indices_t *out,
                                       int *out_count) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "daily");
    int count = cJSON_GetArraySize(arr);
    if (count > WEATHER_INDICES_MAX) {
        count = WEATHER_INDICES_MAX;
    }
    for (int i = 0; i < count; i++) {
        const cJSON *d = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(d)) {
            continue;
        }
        espaperplay_weather_indices_t *dst = &out[i];
        weather_copy_field(d, "date", dst->date, sizeof(dst->date));
        weather_copy_field(d, "type", dst->type, sizeof(dst->type));
        weather_copy_field(d, "name", dst->name, sizeof(dst->name));
        weather_copy_field(d, "level", dst->level, sizeof(dst->level));
        weather_copy_field(d, "category", dst->category, sizeof(dst->category));
        weather_copy_field(d, "text", dst->text, sizeof(dst->text));
    }
    cJSON_Delete(root);
    *out_count = count;
    return ESP_OK;
}

static esp_err_t weather_parse_air(const char *body, espaperplay_weather_air_t *out) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    weather_copy_field(root, "now.pubTime", out->pub_time, sizeof(out->pub_time));
    weather_copy_field(root, "now.aqi", out->aqi, sizeof(out->aqi));
    weather_copy_field(root, "now.level", out->level, sizeof(out->level));
    weather_copy_field(root, "now.category", out->category, sizeof(out->category));
    weather_copy_field(root, "now.primary", out->primary, sizeof(out->primary));
    weather_copy_field(root, "now.pm10", out->pm10, sizeof(out->pm10));
    weather_copy_field(root, "now.pm2p5", out->pm2p5, sizeof(out->pm2p5));
    weather_copy_field(root, "now.no2", out->no2, sizeof(out->no2));
    weather_copy_field(root, "now.so2", out->so2, sizeof(out->so2));
    weather_copy_field(root, "now.co", out->co, sizeof(out->co));
    weather_copy_field(root, "now.o3", out->o3, sizeof(out->o3));
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t weather_parse_astronomy(const char *body, bool sun, espaperplay_weather_astronomy_t *out) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    weather_copy_field(root, "date", out->date, sizeof(out->date));
    if (sun) {
        weather_copy_field(root, "sunrise", out->sunrise, sizeof(out->sunrise));
        weather_copy_field(root, "sunset", out->sunset, sizeof(out->sunset));
    } else {
        weather_copy_field(root, "moonrise", out->moonrise, sizeof(out->moonrise));
        weather_copy_field(root, "moonset", out->moonset, sizeof(out->moonset));
        /* moonPhase 新格式为数组（每小时一条，取当日第一条的月相名 / 图标）；
         * 兼容旧格式（单对象 {text, icon}）。 */
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "moonPhase");
        const cJSON *first = cJSON_GetArrayItem(arr, 0);
        if (cJSON_IsObject(first)) {
            weather_copy_field(first, "name", out->moon_phase, sizeof(out->moon_phase));
            weather_copy_field(first, "icon", out->moon_phase_icon,
                               sizeof(out->moon_phase_icon));
        } else {
            weather_copy_field(root, "moonPhase.text", out->moon_phase, sizeof(out->moon_phase));
            weather_copy_field(root, "moonPhase.icon", out->moon_phase_icon,
                               sizeof(out->moon_phase_icon));
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t weather_parse_lookup(const char *body, espaperplay_weather_location_t *out,
                                      int *out_count) {
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "location");
    int count = cJSON_GetArraySize(arr);
    if (count > WEATHER_LOOKUP_RESULT_MAX) {
        count = WEATHER_LOOKUP_RESULT_MAX;
    }
    for (int i = 0; i < count; i++) {
        const cJSON *l = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(l)) {
            continue;
        }
        espaperplay_weather_location_t *dst = &out[i];
        weather_copy_field(l, "name", dst->name, sizeof(dst->name));
        weather_copy_field(l, "id", dst->id, sizeof(dst->id));
        weather_copy_field(l, "lat", dst->lat, sizeof(dst->lat));
        weather_copy_field(l, "lon", dst->lon, sizeof(dst->lon));
        weather_copy_field(l, "adm1", dst->adm1, sizeof(dst->adm1));
        weather_copy_field(l, "adm2", dst->adm2, sizeof(dst->adm2));
        weather_copy_field(l, "country", dst->country, sizeof(dst->country));
        weather_copy_field(l, "tz", dst->tz, sizeof(dst->tz));
        weather_copy_field(l, "utcOffset", dst->utc_offset, sizeof(dst->utc_offset));
        weather_copy_field(l, "type", dst->type, sizeof(dst->type));
    }
    cJSON_Delete(root);
    *out_count = count;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* URL 编码与请求构造                                                    */
/* ------------------------------------------------------------------ */

/** 对位置参数做 URL 百分号编码（仅保留 RFC 3986 unreserved 字符）。 */
static void weather_url_encode(const char *src, char *dst, size_t dst_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < dst_size; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= dst_size) {
                break;
            }
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
}

/** 构造查询串："location=<编码后的位置>[&额外参数]"。 */
static void weather_build_query(const char *location, const char *extra, char *out,
                                size_t out_size) {
    char enc[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 1];
    weather_url_encode(location, enc, sizeof(enc));
    if (extra != NULL && extra[0] != '\0') {
        snprintf(out, out_size, "location=%s&%s", enc, extra);
    } else {
        snprintf(out, out_size, "location=%s", enc);
    }
}

/**
 * @brief 归一化坐标参数为 QWeather 要求的 "经度,纬度" 格式（最多 2 位小数）。
 *
 * QWeather 要求十进制坐标按 "经度,纬度" 顺序（如 116.41,39.92），且最多
 * 支持小数点后两位；若传入超出范围的纬度（如按习惯写的 "纬度,经度"，
 * 纬度在 [-90,90] 而经度不在该范围），自动交换顺序。
 */
static void weather_normalize_coords(const char *src, char *out, size_t out_size) {
    char compact[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN];
    size_t o = 0;
    for (const char *p = src; *p != '\0' && o + 1 < sizeof(compact); p++) {
        if (*p != ' ' && *p != '\t') {
            compact[o++] = *p;
        }
    }
    compact[o] = '\0';

    double a = 0.0, b = 0.0;
    if (sscanf(compact, "%lf,%lf", &a, &b) != 2) {
        strlcpy(out, compact, out_size);
        return;
    }
    if (a >= -90.0 && a <= 90.0 && (b < -90.0 || b > 90.0)) {
        /* 用户按习惯输入了 "纬度,经度"：交换为 "经度,纬度"。 */
        snprintf(out, out_size, "%.2f,%.2f", b, a);
    } else {
        snprintf(out, out_size, "%.2f,%.2f", a, b);
    }
}

/**
 * @brief 发起一次 QWeather API 请求并校验业务码。
 *
 * @param host    主机（天气数据 API 或 GeoAPI）。
 * @param path    接口路径（如 "/v7/weather/now"）。
 * @param query   查询串（不含 "?"，如 "location=101010100&type=0"）。
 * @param max_len 响应体上限。
 * @param out_body 成功（业务码 200）时输出 malloc 的响应体（调用方负责 free）。
 *
 * @return ESP_OK 成功；API Key 未配置返回 ESP_ERR_INVALID_STATE；业务码 204
 *         返回 ESP_ERR_NOT_SUPPORTED；404 返回 ESP_ERR_NOT_FOUND；其余失败
 *         返回相应错误码。失败时最近错误记录在状态中。
 */
static esp_err_t weather_request(const char *host, const char *path, const char *query,
                                 size_t max_len, char **out_body) {
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();
    if (cfg->weather_api_key[0] == '\0') {
        weather_record_error(0, "和风天气 API Key 未配置");
        return ESP_ERR_INVALID_STATE;
    }

    char url[384];
    snprintf(url, sizeof(url), "%s%s?%s", host, path, query);

    char *body = NULL;
    esp_err_t err = weather_http_get(url, cfg->weather_api_key, max_len, &body);
    if (err != ESP_OK) {
        char msg[sizeof(s_status->last_error)];
        snprintf(msg, sizeof(msg), "%s: %s", path, esp_err_to_name(err));
        weather_record_error(0, msg);
        return err;
    }

    int code = weather_parse_code(body);
    if (code != 200) {
        if (code == 0) {
            /* 业务码解析失败（非标准 JSON 响应），打印响应体前缀便于诊断。 */
            weather_log_body_preview(body, strlen(body));
        }
        weather_record_api_error(path, code);
        free(body);
        if (code == 204) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (code == 404) {
            return ESP_ERR_NOT_FOUND;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    weather_record_error(200, "");
    *out_body = body;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 缓存辅助                                                             */
/* ------------------------------------------------------------------ */

/** 通用缓存命中判断（TTL 内且 LocationID 匹配）。 */
static bool weather_cache_hit(bool valid, uint64_t ts_ms, const char *cache_loc,
                              const char *loc_id, uint32_t ttl_ms) {
    if (!valid || strcmp(cache_loc, loc_id) != 0) {
        return false;
    }
    return weather_now_ms() - ts_ms < ttl_ms;
}

/** 通用缓存写入（调用方需持锁）。 */
static void weather_cache_store(bool *valid, uint64_t *ts_ms, char *cache_loc,
                                const char *loc_id) {
    *valid = true;
    *ts_ms = weather_now_ms();
    strlcpy(cache_loc, loc_id, 16);
}

/* ------------------------------------------------------------------ */
/* 位置解析                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief 解析要查询的位置：优先配置值，其次自动定位（公网 IP -> 经纬度 ->
 * GeoAPI 反查）。输出 LocationID 与显示名称。
 */
static esp_err_t weather_resolve_location(char *loc_id, size_t id_size, char *loc_name,
                                          size_t name_size, bool *auto_location) {
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();

    if (cfg->weather_location[0] != '\0') {
        /* 配置了位置：直接使用。若是 LocationID（纯数字），反查一次友好名称。 */
        strlcpy(loc_id, cfg->weather_location, id_size);
        *auto_location = false;

        bool is_id = true;
        for (const char *p = cfg->weather_location; *p; p++) {
            if (*p < '0' || *p > '9') {
                is_id = false;
                break;
            }
        }
        if (is_id && strlen(cfg->weather_location) <= 10) {
            espaperplay_weather_location_t locs[WEATHER_LOOKUP_RESULT_MAX];
            int count = 0;
            if (espaperplay_weather_location_lookup(cfg->weather_location, locs, &count) ==
                    ESP_OK &&
                count > 0 && locs[0].name[0] != '\0') {
                strlcpy(loc_name, locs[0].name, name_size);
                return ESP_OK;
            }
        }
        strlcpy(loc_name, cfg->weather_location, name_size);
        return ESP_OK;
    }

    /* 自动定位：先查缓存（24 小时 TTL）。 */
    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    bool hit = s_cache_auto_loc != NULL && s_cache_auto_loc->valid &&
               weather_now_ms() - s_cache_auto_loc->ts_ms < ESPAPERPLAY_WEATHER_AUTO_LOC_TTL_MS;
    if (hit) {
        strlcpy(loc_id, s_cache_auto_loc->id, id_size);
        strlcpy(loc_name, s_cache_auto_loc->name, name_size);
        *auto_location = true;
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (hit) {
        ESP_LOGI(TAG, "auto location cache hit: %s (%s)", loc_name, loc_id);
        return ESP_OK;
    }

    /* 公网 IP -> 地理位置（经纬度）-> GeoAPI 反查 LocationID。 */
    char ip[ESPAPERPLAY_NETIP_IP_MAX_LEN];
    esp_err_t err = espaperplay_netip_query(ip, sizeof(ip));
    if (err != ESP_OK) {
        weather_record_error(0, "自动定位失败：获取公网 IP 出错");
        return err;
    }
    espaperplay_geoip_info_t geo;
    err = espaperplay_geoip_query(ip, &geo);
    if (err != ESP_OK) {
        weather_record_error(0, "自动定位失败：获取地理位置出错");
        return err;
    }
    if (!geo.has_coordinates) {
        weather_record_error(0, "自动定位失败：地理位置无经纬度");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* GeoAPI 反查要求 "经度,纬度"（最多 2 位小数）：geoip 返回的经纬度
     * 需交换顺序并截断精度，否则服务端返回 400（纬度 116.xx 非法）。 */
    char query[48];
    snprintf(query, sizeof(query), "%.2f,%.2f", geo.longitude, geo.latitude);
    espaperplay_weather_location_t locs[WEATHER_LOOKUP_RESULT_MAX];
    int count = 0;
    err = espaperplay_weather_location_lookup(query, locs, &count);
    if (err != ESP_OK || count == 0 || locs[0].id[0] == '\0') {
        weather_record_error(0, "自动定位失败：经纬度反查城市失败");
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    strlcpy(loc_id, locs[0].id, id_size);
    if (locs[0].name[0] != '\0') {
        strlcpy(loc_name, locs[0].name, name_size);
    } else if (locs[0].adm1[0] != '\0') {
        strlcpy(loc_name, locs[0].adm1, name_size);
    } else {
        strlcpy(loc_name, locs[0].id, name_size);
    }
    *auto_location = true;

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_cache_auto_loc != NULL) {
        s_cache_auto_loc->valid = true;
        s_cache_auto_loc->ts_ms = weather_now_ms();
        s_cache_auto_loc->auto_location = true;
        strlcpy(s_cache_auto_loc->id, loc_id, sizeof(s_cache_auto_loc->id));
        strlcpy(s_cache_auto_loc->name, loc_name, sizeof(s_cache_auto_loc->name));
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "auto location resolved: %s (%s) from ip %s", loc_name, loc_id, ip);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 单项查询（含各自缓存）                                                */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_weather_location_lookup(const char *query,
                                              espaperplay_weather_location_t *out,
                                              int *out_count) {
    if (query == NULL || query[0] == '\0' || out == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 缓存查找（按查询串）。 */
    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_cache_lookup != NULL) {
        for (int i = 0; i < ESPAPERPLAY_WEATHER_LOOKUP_CACHE_ENTRIES; i++) {
            const weather_lookup_cache_entry_t *e = &s_cache_lookup[i];
            if (e->valid && strcmp(e->query, query) == 0 &&
                weather_now_ms() - e->ts_ms < ESPAPERPLAY_WEATHER_LOOKUP_TTL_MS) {
                *out_count = e->count;
                for (int j = 0; j < e->count; j++) {
                    out[j] = e->loc[j];
                }
                if (s_lock != NULL) {
                    xSemaphoreGive(s_lock);
                }
                return ESP_OK;
            }
        }
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }

    char q[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 1];
    weather_build_query(query, NULL, q, sizeof(q));
    const char *geo_host = NULL;
    const char *geo_path = NULL;
    const char *data_host = NULL;
    weather_get_hosts(&data_host, &geo_host, &geo_path);
    char *body = NULL;
    esp_err_t err = weather_request(geo_host, geo_path, q, ESPAPERPLAY_WEATHER_RESP_LOOKUP_MAX,
                                    &body);
    if (err != ESP_OK) {
        return err;
    }
    espaperplay_weather_location_t tmp[WEATHER_LOOKUP_RESULT_MAX];
    int count = 0;
    err = weather_parse_lookup(body, tmp, &count);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    /* 写入缓存：覆盖同查询串条目，否则空槽 / 最旧淘汰。 */
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_cache_lookup != NULL) {
        int slot = -1;
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < ESPAPERPLAY_WEATHER_LOOKUP_CACHE_ENTRIES; i++) {
            if (s_cache_lookup[i].valid && strcmp(s_cache_lookup[i].query, query) == 0) {
                slot = i;
                break;
            }
            if (!s_cache_lookup[i].valid) {
                slot = i;
                break;
            }
            if (s_cache_lookup[i].ts_ms < oldest) {
                oldest = s_cache_lookup[i].ts_ms;
                slot = i;
            }
        }
        if (slot >= 0) {
            weather_lookup_cache_entry_t *e = &s_cache_lookup[slot];
            e->valid = true;
            e->ts_ms = weather_now_ms();
            strlcpy(e->query, query, sizeof(e->query));
            e->count = count;
            for (int j = 0; j < count; j++) {
                e->loc[j] = tmp[j];
            }
        }
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }

    for (int j = 0; j < count; j++) {
        out[j] = tmp[j];
    }
    *out_count = count;
    ESP_LOGI(TAG, "location lookup \"%s\": %d result(s)", query, count);
    return ESP_OK;
}

/** 解析查询位置参数：NULL -> 使用配置 / 自动定位。 */
static esp_err_t weather_effective_location(const char *location, char *loc_id, size_t id_size) {
    if (location != NULL && location[0] != '\0') {
        /* 直接传参（LocationID / 城市名 / "经度,纬度"）：原样使用，
         * 但城市名 / 坐标先经 GeoAPI 解析为 LocationID，保证缓存键一致。 */
        bool is_id = true;
        bool is_coord = false;
        int dots = 0;
        for (const char *p = location; *p; p++) {
            if (*p == ',') {
                dots++;
            } else if ((*p < '0' || *p > '9') && *p != '.' && *p != '-') {
                is_id = false;
            }
        }
        is_coord = (dots == 1);
        if (is_id && is_coord) {
            /* "经度,纬度"（兼容 "纬度,经度" 自动交换）：反查为 LocationID。 */
            char norm[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN];
            weather_normalize_coords(location, norm, sizeof(norm));
            espaperplay_weather_location_t locs[WEATHER_LOOKUP_RESULT_MAX];
            int count = 0;
            if (espaperplay_weather_location_lookup(norm, locs, &count) == ESP_OK &&
                count > 0 && locs[0].id[0] != '\0') {
                strlcpy(loc_id, locs[0].id, id_size);
                return ESP_OK;
            }
            return ESP_ERR_NOT_FOUND;
        }
        if (is_id && !is_coord) {
            strlcpy(loc_id, location, id_size);
            return ESP_OK;
        }
        /* 城市名等：GeoAPI 解析为 LocationID。 */
        espaperplay_weather_location_t locs[WEATHER_LOOKUP_RESULT_MAX];
        int count = 0;
        if (espaperplay_weather_location_lookup(location, locs, &count) == ESP_OK && count > 0 &&
            locs[0].id[0] != '\0') {
            strlcpy(loc_id, locs[0].id, id_size);
            return ESP_OK;
        }
        return ESP_ERR_NOT_FOUND;
    }
    /* NULL / 空串：配置位置或自动定位。 */
    char name[128];
    bool auto_loc = false;
    return weather_resolve_location(loc_id, id_size, name, sizeof(name), &auto_loc);
}

static esp_err_t weather_fetch_now(const char *location, espaperplay_weather_now_t *out) {
    char loc_id[16];
    esp_err_t err = weather_effective_location(location, loc_id, sizeof(loc_id));
    if (err != ESP_OK) {
        weather_record_error(0, "位置解析失败");
        return err;
    }

    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    bool hit = s_cache_now != NULL &&
               weather_cache_hit(s_cache_now->valid, s_cache_now->ts_ms, s_cache_now->loc, loc_id,
                                 ESPAPERPLAY_WEATHER_TTL_NOW_MS);
    if (hit) {
        *out = s_cache_now->data;
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (hit) {
        return ESP_OK;
    }

    char query[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 32];
    weather_build_query(loc_id, NULL, query, sizeof(query));
    char *body = NULL;
    err = weather_request(weather_data_host(), "/v7/weather/now", query,
                          ESPAPERPLAY_WEATHER_RESP_NOW_MAX, &body);
    if (err != ESP_OK) {
        return err;
    }
    espaperplay_weather_now_t tmp;
    err = weather_parse_now(body, &tmp);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_cache_now != NULL) {
        s_cache_now->data = tmp;
        weather_cache_store(&s_cache_now->valid, &s_cache_now->ts_ms, s_cache_now->loc, loc_id);
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    *out = tmp;
    return ESP_OK;
}

static esp_err_t weather_fetch_daily(const char *location, int days,
                                     espaperplay_weather_daily_t *out, int *out_count) {
    if (days != 3 && days != 7) {
        return ESP_ERR_INVALID_ARG;
    }
    char loc_id[16];
    esp_err_t err = weather_effective_location(location, loc_id, sizeof(loc_id));
    if (err != ESP_OK) {
        weather_record_error(0, "位置解析失败");
        return err;
    }

    weather_daily_cache_t *cache = (days == 3) ? s_cache_daily_3d : s_cache_daily_7d;
    uint32_t ttl = ESPAPERPLAY_WEATHER_TTL_DAILY_MS;

    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    bool hit = cache != NULL &&
               weather_cache_hit(cache->valid, cache->ts_ms, cache->loc, loc_id, ttl);
    if (hit) {
        *out_count = cache->count;
        for (int i = 0; i < cache->count; i++) {
            out[i] = cache->data[i];
        }
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (hit) {
        return ESP_OK;
    }

    char query[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 32];
    weather_build_query(loc_id, NULL, query, sizeof(query));
    const char *path = (days == 3) ? "/v7/weather/3d" : "/v7/weather/7d";
    char *body = NULL;
    err = weather_request(weather_data_host(), path, query,
                          ESPAPERPLAY_WEATHER_RESP_DAILY_MAX, &body);
    if (err != ESP_OK) {
        return err;
    }
    /* 直接解析进调用方缓冲（大数组由调用方在堆上分配，避免任务栈溢出）。 */
    int count = 0;
    err = weather_parse_daily(body, out, &count);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (cache != NULL) {
        cache->count = count;
        for (int i = 0; i < count; i++) {
            cache->data[i] = out[i];
        }
        weather_cache_store(&cache->valid, &cache->ts_ms, cache->loc, loc_id);
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }

    *out_count = count;
    return ESP_OK;
}

static esp_err_t weather_fetch_hourly(const char *location, espaperplay_weather_hourly_t *out,
                                      int *out_count) {
    char loc_id[16];
    esp_err_t err = weather_effective_location(location, loc_id, sizeof(loc_id));
    if (err != ESP_OK) {
        weather_record_error(0, "位置解析失败");
        return err;
    }

    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    bool hit = s_cache_hourly != NULL &&
               weather_cache_hit(s_cache_hourly->valid, s_cache_hourly->ts_ms, s_cache_hourly->loc,
                                 loc_id, ESPAPERPLAY_WEATHER_TTL_HOURLY_MS);
    if (hit) {
        *out_count = s_cache_hourly->count;
        for (int i = 0; i < s_cache_hourly->count; i++) {
            out[i] = s_cache_hourly->data[i];
        }
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (hit) {
        return ESP_OK;
    }

    char query[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 32];
    weather_build_query(loc_id, NULL, query, sizeof(query));
    char *body = NULL;
    err = weather_request(weather_data_host(), "/v7/weather/24h", query,
                          ESPAPERPLAY_WEATHER_RESP_HOURLY_MAX, &body);
    if (err != ESP_OK) {
        return err;
    }
    /* 直接解析进调用方缓冲（大数组由调用方在堆上分配，避免任务栈溢出）。 */
    int count = 0;
    err = weather_parse_hourly(body, out, &count);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_cache_hourly != NULL) {
        s_cache_hourly->count = count;
        for (int i = 0; i < count; i++) {
            s_cache_hourly->data[i] = out[i];
        }
        weather_cache_store(&s_cache_hourly->valid, &s_cache_hourly->ts_ms, s_cache_hourly->loc,
                            loc_id);
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }

    *out_count = count;
    return ESP_OK;
}

static esp_err_t weather_fetch_minutely(const char *location, espaperplay_weather_minutely_t *out) {
    char loc_id[16];
    esp_err_t err = weather_effective_location(location, loc_id, sizeof(loc_id));
    if (err != ESP_OK) {
        weather_record_error(0, "位置解析失败");
        return err;
    }

    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    bool hit = s_cache_minutely != NULL &&
               weather_cache_hit(s_cache_minutely->valid, s_cache_minutely->ts_ms,
                                 s_cache_minutely->loc, loc_id,
                                 ESPAPERPLAY_WEATHER_TTL_MINUTELY_MS);
    if (hit) {
        *out = s_cache_minutely->data;
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (hit) {
        return ESP_OK;
    }

    /* 分钟级降水接口只接受 "经度,纬度" 坐标（新平台不再支持 LocationID），
     * 通过 GeoAPI 反查 LocationID 对应的坐标（带 24h 缓存）。 */
    char coord[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN];
    {
        espaperplay_weather_location_t locs[WEATHER_LOOKUP_RESULT_MAX];
        int count = 0;
        if (espaperplay_weather_location_lookup(loc_id, locs, &count) != ESP_OK || count == 0 ||
            locs[0].lat[0] == '\0' || locs[0].lon[0] == '\0') {
            weather_record_error(0, "分钟级降水：无法解析位置坐标");
            return ESP_ERR_NOT_FOUND;
        }
        char raw[48];
        snprintf(raw, sizeof(raw), "%s,%s", locs[0].lat, locs[0].lon);
        weather_normalize_coords(raw, coord, sizeof(coord));
    }

    char query[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 32];
    weather_build_query(coord, NULL, query, sizeof(query));
    char *body = NULL;
    err = weather_request(weather_data_host(), "/v7/minutely/5m", query,
                          ESPAPERPLAY_WEATHER_RESP_MINUTELY_MAX, &body);
    if (err != ESP_OK) {
        return err;
    }
    /* 直接解析进调用方缓冲（避免大结构体占用任务栈）。 */
    err = weather_parse_minutely(body, out);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_cache_minutely != NULL) {
        s_cache_minutely->data = *out;
        weather_cache_store(&s_cache_minutely->valid, &s_cache_minutely->ts_ms,
                            s_cache_minutely->loc, loc_id);
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

static esp_err_t weather_fetch_warning(const char *location, espaperplay_weather_warning_t *out,
                                       int *out_count) {
    char loc_id[16];
    esp_err_t err = weather_effective_location(location, loc_id, sizeof(loc_id));
    if (err != ESP_OK) {
        weather_record_error(0, "位置解析失败");
        return err;
    }

    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    bool hit = s_cache_warning != NULL &&
               weather_cache_hit(s_cache_warning->valid, s_cache_warning->ts_ms,
                                 s_cache_warning->loc, loc_id,
                                 ESPAPERPLAY_WEATHER_TTL_WARNING_MS);
    if (hit) {
        *out_count = s_cache_warning->count;
        for (int i = 0; i < s_cache_warning->count; i++) {
            out[i] = s_cache_warning->data[i];
        }
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (hit) {
        return ESP_OK;
    }

    char query[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 32];
    weather_build_query(loc_id, NULL, query, sizeof(query));
    char *body = NULL;
    err = weather_request(weather_data_host(), "/v7/warning/now", query,
                          ESPAPERPLAY_WEATHER_RESP_WARNING_MAX, &body);
    if (err != ESP_OK) {
        return err;
    }
    /* 直接解析进调用方缓冲（大数组由调用方在堆上分配，避免任务栈溢出）。 */
    int count = 0;
    err = weather_parse_warning(body, out, &count);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_cache_warning != NULL) {
        s_cache_warning->count = count;
        for (int i = 0; i < count; i++) {
            s_cache_warning->data[i] = out[i];
        }
        weather_cache_store(&s_cache_warning->valid, &s_cache_warning->ts_ms,
                            s_cache_warning->loc, loc_id);
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }

    *out_count = count;
    return ESP_OK;
}

static esp_err_t weather_fetch_indices(const char *location, const char *type,
                                       espaperplay_weather_indices_t *out, int *out_count) {
    /* 仅对「全部指数」请求（type 为空或 "0"）使用缓存，其余实时请求。 */
    bool use_cache = (type == NULL || type[0] == '\0' || strcmp(type, "0") == 0);

    char loc_id[16];
    esp_err_t err = weather_effective_location(location, loc_id, sizeof(loc_id));
    if (err != ESP_OK) {
        weather_record_error(0, "位置解析失败");
        return err;
    }

    if (use_cache) {
        weather_lock_ensure();
        if (s_lock != NULL) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        bool hit = s_cache_indices != NULL &&
                   weather_cache_hit(s_cache_indices->valid, s_cache_indices->ts_ms,
                                     s_cache_indices->loc, loc_id,
                                     ESPAPERPLAY_WEATHER_TTL_INDICES_MS);
        if (hit) {
            *out_count = s_cache_indices->count;
            for (int i = 0; i < s_cache_indices->count; i++) {
                out[i] = s_cache_indices->data[i];
            }
        }
        if (s_lock != NULL) {
            xSemaphoreGive(s_lock);
        }
        if (hit) {
            return ESP_OK;
        }
    }

    char query[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 32];
    char extra[16];
    if (use_cache) {
        strlcpy(extra, "type=0", sizeof(extra));
    } else {
        snprintf(extra, sizeof(extra), "type=%s", type != NULL ? type : "0");
    }
    weather_build_query(loc_id, extra, query, sizeof(query));
    char *body = NULL;
    err = weather_request(weather_data_host(), "/v7/indices/1d", query,
                          ESPAPERPLAY_WEATHER_RESP_INDICES_MAX, &body);
    if (err != ESP_OK) {
        return err;
    }
    /* 直接解析进调用方缓冲（大数组由调用方在堆上分配，避免任务栈溢出）。 */
    int count = 0;
    err = weather_parse_indices(body, out, &count);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    if (use_cache) {
        if (s_lock != NULL) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        if (s_cache_indices != NULL) {
            s_cache_indices->count = count;
            for (int i = 0; i < count; i++) {
                s_cache_indices->data[i] = out[i];
            }
            weather_cache_store(&s_cache_indices->valid, &s_cache_indices->ts_ms,
                                s_cache_indices->loc, loc_id);
        }
        if (s_lock != NULL) {
            xSemaphoreGive(s_lock);
        }
    }

    *out_count = count;
    return ESP_OK;
}

static esp_err_t weather_fetch_air(const char *location, espaperplay_weather_air_t *out) {
    char loc_id[16];
    esp_err_t err = weather_effective_location(location, loc_id, sizeof(loc_id));
    if (err != ESP_OK) {
        weather_record_error(0, "位置解析失败");
        return err;
    }

    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    bool hit = s_cache_air != NULL &&
               weather_cache_hit(s_cache_air->valid, s_cache_air->ts_ms, s_cache_air->loc, loc_id,
                                 ESPAPERPLAY_WEATHER_TTL_AIR_MS);
    if (hit) {
        *out = s_cache_air->data;
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (hit) {
        return ESP_OK;
    }

    char query[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 32];
    weather_build_query(loc_id, NULL, query, sizeof(query));
    char *body = NULL;
    err = weather_request(weather_data_host(), "/v7/air/now", query,
                          ESPAPERPLAY_WEATHER_RESP_AIR_MAX, &body);
    if (err != ESP_OK) {
        return err;
    }
    espaperplay_weather_air_t tmp;
    err = weather_parse_air(body, &tmp);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_cache_air != NULL) {
        s_cache_air->data = tmp;
        weather_cache_store(&s_cache_air->valid, &s_cache_air->ts_ms, s_cache_air->loc, loc_id);
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    *out = tmp;
    return ESP_OK;
}

/** 以今天为基准偏移 days 天，输出 yyyyMMdd（本地时区，无夏令时环境）。 */
static void weather_rel_date(int days, char *out, size_t out_size) {
    time_t t = time(NULL) + (time_t)days * 86400;
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    strftime(out, out_size, "%Y%m%d", &tm_local);
}

/** 请求指定日期（yyyyMMdd）的月升/月落，成功且字段非空时回填 out。
 * 仅取对应字段（该日 moonPhase 等不回传）；失败不影响调用方今日数据。 */
static void weather_fetch_moon_field(const char *loc_id, const char *date, bool rise,
                                     char *out, size_t out_size) {
    char extra[24];
    snprintf(extra, sizeof(extra), "date=%s", date);
    char query[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 32];
    weather_build_query(loc_id, extra, query, sizeof(query));
    char *body = NULL;
    espaperplay_weather_astronomy_t tmp = {0};
    esp_err_t err = weather_request(weather_data_host(), "/v7/astronomy/moon", query,
                                    ESPAPERPLAY_WEATHER_RESP_ASTRONOMY_MAX, &body);
    if (err == ESP_OK) {
        err = weather_parse_astronomy(body, false, &tmp);
    }
    free(body);
    const char *src = rise ? tmp.moonrise : tmp.moonset;
    if (err == ESP_OK && src[0] != '\0') {
        strlcpy(out, src, out_size);
        ESP_LOGI(TAG, "backfilled %s from %s: %s", rise ? "moonrise" : "moonset", date, src);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "moon field fetch for %s failed: %s", date, esp_err_to_name(err));
    }
}

/*! 月亮地平线上平均时长（约 12h25m）：月出/月落缺失且无真实值可回填时，
 * 以配对事件粗推邻日时间（无月出日必有月落，反之亦然）。该时长随月赤纬
 * 与纬度在约 11~14.5h 间浮动，估算误差可至 ±1.5h，仅保证弧线月亮图标
 * 不缺席。 */
#define WEATHER_MOON_ABOVE_MIN (12 * 60 + 25)

/** 解析天文时间戳（"2026-08-16T05:12+08:00" 或短格式 "05:12"）为分钟数。 */
static int weather_moon_ts_to_min(const char *ts) {
    const char *t = (ts != NULL) ? strchr(ts, 'T') : NULL;
    t = (t != NULL) ? t + 1 : ts;
    int h = 0, m = 0;
    if (t == NULL || sscanf(t, "%d:%d", &h, &m) != 2) {
        return -1;
    }
    return h * 60 + m;
}

static esp_err_t weather_fetch_astronomy(const char *location,
                                         espaperplay_weather_astronomy_t *out) {
    char loc_id[16];
    esp_err_t err = weather_effective_location(location, loc_id, sizeof(loc_id));
    if (err != ESP_OK) {
        weather_record_error(0, "位置解析失败");
        return err;
    }

    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    bool hit = s_cache_astronomy != NULL &&
               weather_cache_hit(s_cache_astronomy->valid, s_cache_astronomy->ts_ms,
                                 s_cache_astronomy->loc, loc_id,
                                 ESPAPERPLAY_WEATHER_TTL_ASTRONOMY_MS);
    if (hit) {
        *out = s_cache_astronomy->data;
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (hit) {
        return ESP_OK;
    }

    /* 天文接口要求必选 date 参数（yyyyMMdd，最多未来 60 天），取本地当天日期。 */
    char date[16];
    {
        time_t t = time(NULL);
        struct tm tm_local;
        localtime_r(&t, &tm_local);
        strftime(date, sizeof(date), "%Y%m%d", &tm_local);
    }
    char extra[24];
    snprintf(extra, sizeof(extra), "date=%s", date);

    char query[ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN * 3 + 32];
    weather_build_query(loc_id, extra, query, sizeof(query));
    char *body = NULL;
    err = weather_request(weather_data_host(), "/v7/astronomy/sun", query,
                          ESPAPERPLAY_WEATHER_RESP_ASTRONOMY_MAX, &body);
    if (err != ESP_OK) {
        return err;
    }
    /* 必须清零：moonrise_prev/moonset_next 无解析路径覆盖，不清零会把栈
     * 垃圾带进快照（曾渲染出"月出 昨+08:00"）。 */
    espaperplay_weather_astronomy_t tmp = {0};
    err = weather_parse_astronomy(body, true, &tmp);
    free(body);
    if (err != ESP_OK) {
        return err;
    }
    err = weather_request(weather_data_host(), "/v7/astronomy/moon", query,
                          ESPAPERPLAY_WEATHER_RESP_ASTRONOMY_MAX, &body);
    if (err != ESP_OK) {
        return err;
    }
    err = weather_parse_astronomy(body, false, &tmp);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    /* 今日无月出/月落（月升月落每日推迟约 50 分钟，跨过午夜的那天该事件记入
     * 前后日，和风当日字段返回空串，每朔望月各约 1-2 天）时回填，优先级：
     * 真实值（上次月出缓存 / 相邻日请求，date 仅支持今日起未来 60 天）→
     * 配对事件粗推（见 WEATHER_MOON_ABOVE_MIN）→ 留空显示 "--"。
     * 回填失败不影响今日数据；正常日子不多花请求。 */
    if (tmp.moonrise[0] == '\0') {
        if (s_cache_astronomy != NULL && s_cache_astronomy->last_moonrise[0] != '\0') {
            strlcpy(tmp.moonrise_prev, s_cache_astronomy->last_moonrise,
                    sizeof(tmp.moonrise_prev));
            ESP_LOGI(TAG, "moonrise empty today, backfilled from cache: %s",
                     tmp.moonrise_prev);
        } else {
            const int ms_min = weather_moon_ts_to_min(tmp.moonset);
            if (ms_min >= 0) {
                const int est = ((ms_min - WEATHER_MOON_ABOVE_MIN) % 1440 + 1440) % 1440;
                snprintf(tmp.moonrise_prev, sizeof(tmp.moonrise_prev), "%02d:%02d",
                         est / 60, est % 60);
                ESP_LOGW(TAG, "moonrise empty, estimated from moonset %s: %s", tmp.moonset,
                         tmp.moonrise_prev);
            }
        }
    }
    if (tmp.moonset[0] == '\0') {
        char next_date[16];
        weather_rel_date(+1, next_date, sizeof(next_date));
        weather_fetch_moon_field(loc_id, next_date, false, tmp.moonset_next,
                                 sizeof(tmp.moonset_next));
        if (tmp.moonset_next[0] == '\0') {
            const int mr_min = weather_moon_ts_to_min(tmp.moonrise);
            if (mr_min >= 0) {
                const int est = (mr_min + WEATHER_MOON_ABOVE_MIN) % 1440;
                snprintf(tmp.moonset_next, sizeof(tmp.moonset_next), "%02d:%02d",
                         est / 60, est % 60);
                ESP_LOGW(TAG, "moonset empty, estimated from moonrise %s: %s", tmp.moonrise,
                         tmp.moonset_next);
            }
        }
    }

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_cache_astronomy != NULL) {
        s_cache_astronomy->data = tmp;
        if (tmp.moonrise[0] != '\0') {
            strlcpy(s_cache_astronomy->last_moonrise, tmp.moonrise,
                    sizeof(s_cache_astronomy->last_moonrise));
        }
        weather_cache_store(&s_cache_astronomy->valid, &s_cache_astronomy->ts_ms,
                            s_cache_astronomy->loc, loc_id);
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    *out = tmp;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 公开查询接口                                                         */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_weather_query_now(const char *location, espaperplay_weather_now_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return weather_fetch_now(location, out);
}

esp_err_t espaperplay_weather_query_daily(const char *location, int days,
                                          espaperplay_weather_daily_t *out, int *out_count) {
    if (out == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return weather_fetch_daily(location, days, out, out_count);
}

esp_err_t espaperplay_weather_query_hourly(const char *location,
                                           espaperplay_weather_hourly_t *out, int *out_count) {
    if (out == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return weather_fetch_hourly(location, out, out_count);
}

esp_err_t espaperplay_weather_query_minutely(const char *location,
                                             espaperplay_weather_minutely_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return weather_fetch_minutely(location, out);
}

esp_err_t espaperplay_weather_query_warning(const char *location,
                                            espaperplay_weather_warning_t *out, int *out_count) {
    if (out == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return weather_fetch_warning(location, out, out_count);
}

esp_err_t espaperplay_weather_query_indices(const char *location, const char *type,
                                            espaperplay_weather_indices_t *out, int *out_count) {
    if (out == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return weather_fetch_indices(location, type, out, out_count);
}

esp_err_t espaperplay_weather_query_air(const char *location, espaperplay_weather_air_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return weather_fetch_air(location, out);
}

esp_err_t espaperplay_weather_query_astronomy(const char *location,
                                              espaperplay_weather_astronomy_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return weather_fetch_astronomy(location, out);
}

/* ------------------------------------------------------------------ */
/* 快照刷新                                                             */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_weather_refresh(void) {
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();
    if (cfg->weather_api_key[0] == '\0') {
        weather_record_error(0, "和风天气 API Key 未配置");
        return ESP_ERR_INVALID_STATE;
    }
    s_last_refresh_try_ms = esp_timer_get_time() / 1000LL;

    /* 1. 解析 / 复用位置。 */
    char loc_id[16];
    char loc_name[128];
    bool auto_loc = false;
    esp_err_t err = weather_resolve_location(loc_id, sizeof(loc_id), loc_name, sizeof(loc_name),
                                             &auto_loc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "location resolve failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "refresh for location %s (%s, auto=%d)", loc_id, loc_name, auto_loc);

    /* 2. 逐接口刷新（各自受 TTL 约束；先取到堆上缓冲，最后统一提交快照，
     *    避免多线程下快照字段与元数据不一致）。单项失败不影响其余。
     *    注意：全部数据缓冲在堆上分配——结构体数组合计约 22KB，若放任务
     *    栈上会直接栈溢出（曾导致配置 Key 后循环崩溃）。 */
    espaperplay_weather_now_t *now = calloc(1, sizeof(*now));
    espaperplay_weather_daily_t *daily = calloc(WEATHER_DAILY_MAX, sizeof(*daily));
    espaperplay_weather_hourly_t *hourly = calloc(WEATHER_HOURLY_MAX, sizeof(*hourly));
    espaperplay_weather_minutely_t *minutely = calloc(1, sizeof(*minutely));
    espaperplay_weather_warning_t *warnings = calloc(WEATHER_WARNING_MAX, sizeof(*warnings));
    espaperplay_weather_indices_t *indices = calloc(WEATHER_INDICES_MAX, sizeof(*indices));
    espaperplay_weather_air_t *air = calloc(1, sizeof(*air));
    espaperplay_weather_astronomy_t *astronomy = calloc(1, sizeof(*astronomy));
    if (now == NULL || daily == NULL || hourly == NULL || minutely == NULL || warnings == NULL ||
        indices == NULL || air == NULL || astronomy == NULL) {
        ESP_LOGE(TAG, "refresh: out of memory for data buffers");
        free(now);
        free(daily);
        free(hourly);
        free(minutely);
        free(warnings);
        free(indices);
        free(air);
        free(astronomy);
        return ESP_ERR_NO_MEM;
    }

    int daily_count = 0;
    int hourly_count = 0;
    int warning_count = 0;
    int indices_count = 0;

    bool any_ok = false;

    err = weather_fetch_now(loc_id, now);
    if (err == ESP_OK) {
        any_ok = true;
    } else {
        ESP_LOGW(TAG, "now failed: %s", esp_err_to_name(err));
    }

    err = weather_fetch_daily(loc_id, 7, daily, &daily_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "daily(7d) failed: %s", esp_err_to_name(err));
        /* 7 日预报失败（如订阅不含）时回退 3 日预报。 */
        if (weather_fetch_daily(loc_id, 3, daily, &daily_count) != ESP_OK) {
            daily_count = 0;
            ESP_LOGW(TAG, "daily(3d) fallback failed too");
        }
    }

    err = weather_fetch_hourly(loc_id, hourly, &hourly_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hourly failed: %s", esp_err_to_name(err));
        hourly_count = 0;
    }

    err = weather_fetch_minutely(loc_id, minutely);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "minutely failed: %s", esp_err_to_name(err));
    }

    err = weather_fetch_warning(loc_id, warnings, &warning_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "warning failed: %s", esp_err_to_name(err));
        warning_count = 0;
    }

    err = weather_fetch_indices(loc_id, NULL, indices, &indices_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "indices failed: %s", esp_err_to_name(err));
        indices_count = 0;
    }

    err = weather_fetch_air(loc_id, air);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "air failed: %s", esp_err_to_name(err));
    }

    err = weather_fetch_astronomy(loc_id, astronomy);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "astronomy failed: %s", esp_err_to_name(err));
    }

    if (!any_ok) {
        ESP_LOGE(TAG, "weather refresh failed: no data retrieved");
        free(now);
        free(daily);
        free(hourly);
        free(minutely);
        free(warnings);
        free(indices);
        free(air);
        free(astronomy);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 3. 统一提交快照与状态。 */
    char update_time[32];
    if (now->obs_time[0] != '\0') {
        strlcpy(update_time, now->obs_time, sizeof(update_time));
    } else {
        time_t t = time(NULL);
        struct tm tm_local;
        localtime_r(&t, &tm_local);
        strftime(update_time, sizeof(update_time), "%Y-%m-%d %H:%M", &tm_local);
    }

    weather_lock_ensure();
    if (s_snapshot == NULL) {
        /* 快照缓冲未就绪（内存不足）：数据已写入各接口缓存，仅快照聚合缺席。 */
        ESP_LOGE(TAG, "snapshot buffer unavailable (out of memory)");
        free(now);
        free(daily);
        free(hourly);
        free(minutely);
        free(warnings);
        free(indices);
        free(air);
        free(astronomy);
        return ESP_ERR_NO_MEM;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_snapshot->valid = true;
    s_snapshot->auto_location = auto_loc;
    strlcpy(s_snapshot->location_id, loc_id, sizeof(s_snapshot->location_id));
    strlcpy(s_snapshot->location_name, loc_name, sizeof(s_snapshot->location_name));
    strlcpy(s_snapshot->update_time, update_time, sizeof(s_snapshot->update_time));
    s_snapshot->now = *now;
    s_snapshot->daily_count = daily_count;
    for (int i = 0; i < daily_count; i++) {
        s_snapshot->daily[i] = daily[i];
    }
    s_snapshot->hourly_count = hourly_count;
    for (int i = 0; i < hourly_count; i++) {
        s_snapshot->hourly[i] = hourly[i];
    }
    s_snapshot->minutely = *minutely;
    s_snapshot->warning_count = warning_count;
    for (int i = 0; i < warning_count; i++) {
        s_snapshot->warnings[i] = warnings[i];
    }
    s_snapshot->indices_count = indices_count;
    for (int i = 0; i < indices_count; i++) {
        s_snapshot->indices[i] = indices[i];
    }
    s_snapshot->air = *air;
    s_snapshot->astronomy = *astronomy;
    if (s_status != NULL) {
        s_status->configured = true;
        s_status->valid = true;
        s_status->auto_location = auto_loc;
        strlcpy(s_status->location_id, loc_id, sizeof(s_status->location_id));
        strlcpy(s_status->location_name, loc_name, sizeof(s_status->location_name));
        strlcpy(s_status->update_time, update_time, sizeof(s_status->update_time));
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "weather refresh done: %s (%s), temp %s°C %s, daily %d, warnings %d",
             loc_name, loc_id, now->temp, now->text, daily_count, warning_count);

    free(now);
    free(daily);
    free(hourly);
    free(minutely);
    free(warnings);
    free(indices);
    free(air);
    free(astronomy);
    return ESP_OK;
}

esp_err_t espaperplay_weather_get_snapshot(espaperplay_weather_snapshot_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    weather_lock_ensure();
    if (s_snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    *out = *s_snapshot;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

esp_err_t espaperplay_weather_get_status(espaperplay_weather_status_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_status != NULL) {
        *out = *s_status;
    } else {
        memset(out, 0, sizeof(*out));
    }
    out->task_running = (s_task != NULL);
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    /* 是否已配置以系统配置为准（刷新前即有效）。 */
    out->configured = espaperplay_system_get_config()->weather_api_key[0] != '\0';
    return ESP_OK;
}

void espaperplay_weather_cache_clear(void) {
    weather_lock_ensure();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_cache_now != NULL) {
        s_cache_now->valid = false;
    }
    if (s_cache_daily_3d != NULL) {
        s_cache_daily_3d->valid = false;
    }
    if (s_cache_daily_7d != NULL) {
        s_cache_daily_7d->valid = false;
    }
    if (s_cache_hourly != NULL) {
        s_cache_hourly->valid = false;
    }
    if (s_cache_minutely != NULL) {
        s_cache_minutely->valid = false;
    }
    if (s_cache_warning != NULL) {
        s_cache_warning->valid = false;
    }
    if (s_cache_indices != NULL) {
        s_cache_indices->valid = false;
    }
    if (s_cache_air != NULL) {
        s_cache_air->valid = false;
    }
    if (s_cache_astronomy != NULL) {
        s_cache_astronomy->valid = false;
    }
    if (s_cache_auto_loc != NULL) {
        s_cache_auto_loc->valid = false;
    }
    if (s_cache_lookup != NULL) {
        for (int i = 0; i < ESPAPERPLAY_WEATHER_LOOKUP_CACHE_ENTRIES; i++) {
            s_cache_lookup[i].valid = false;
        }
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "caches cleared");
}

/* ------------------------------------------------------------------ */
/* 后台任务                                                             */
/* ------------------------------------------------------------------ */

/** 判断设备是否处于已联网的 STA 模式。 */
static bool weather_wifi_sta_online(void) {
    espaperplay_wifi_status_t status;
    return espaperplay_wifi_get_status(&status) == ESP_OK && status.started && status.connected &&
           status.mode == ESPAPERPLAY_WIFI_MODE_STA;
}

/** 后台刷新任务：等待联网后按周期刷新，可被 xTaskNotify 唤醒立即刷新。 */
static void weather_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "weather task started");

    /* 启动时等待 STA 联网（超时后仍进入主循环，断网时自动空转）。 */
    int waited = 0;
    while (waited < ESPAPERPLAY_WEATHER_WIFI_WAIT_MS && !weather_wifi_sta_online()) {
        vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_WEATHER_WIFI_POLL_MS));
        waited += ESPAPERPLAY_WEATHER_WIFI_POLL_MS;
    }
    if (!weather_wifi_sta_online()) {
        ESP_LOGW(TAG, "no STA network at startup, weather will be fetched once online");
    }

    while (1) {
        /* 清除完成信号后刷新，结束时置位：供电源管理等外部方同步等待。 */
        if (s_refresh_done != NULL) {
            xEventGroupClearBits(s_refresh_done, BIT(0));
        }
        if (weather_wifi_sta_online()) {
            esp_err_t err = espaperplay_weather_refresh();
            if (err == ESP_OK) {
                s_last_refresh_ok_ms = esp_timer_get_time() / 1000LL;
            } else {
                ESP_LOGW(TAG, "weather refresh failed: %s", esp_err_to_name(err));
            }
            /* 监控任务栈余量（TLS 握手等路径的栈占用），便于发现潜在溢出。 */
            ESP_LOGD(TAG, "task stack high water: %u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else {
            ESP_LOGD(TAG, "no STA network, skip weather refresh");
        }
        if (s_refresh_done != NULL) {
            xEventGroupSetBits(s_refresh_done, BIT(0));
        }
        /* 等待周期或立即刷新通知（通知返回 pdTRUE，立即进入下一轮）。 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_refresh_interval_ms));
    }
}

esp_err_t espaperplay_weather_start(void) {
    weather_lock_ensure();
    if (s_refresh_done == NULL) {
        s_refresh_done = xEventGroupCreate();
    }
    if (s_task != NULL) {
        return ESP_OK; /* 幂等 */
    }
    if (xTaskCreate(weather_task, "weather_task", ESPAPERPLAY_WEATHER_TASK_STACK_SIZE, NULL,
                    ESPAPERPLAY_WEATHER_TASK_PRIORITY, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create weather task");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "weather task created");
    return ESP_OK;
}

void espaperplay_weather_set_refresh_interval_ms(uint32_t interval_ms) {
    if (interval_ms == 0) {
        interval_ms = ESPAPERPLAY_WEATHER_REFRESH_INTERVAL_MS;
    }
    s_refresh_interval_ms = interval_ms;
    ESP_LOGI(TAG, "refresh interval set to %u ms", (unsigned)interval_ms);
}

void espaperplay_weather_request_refresh(void) {
    weather_lock_ensure();
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

void espaperplay_weather_config_changed(void) {
    ESP_LOGI(TAG, "weather config changed, clearing caches and requesting refresh");
    espaperplay_weather_cache_clear();
    espaperplay_weather_request_refresh();
}

bool espaperplay_weather_is_refresh_due(void) {
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();
    if (cfg->weather_api_key[0] == '\0') {
        return false; /* 未配置 Key：永不因天气到期而触发联网 */
    }
    const int64_t now_ms = esp_timer_get_time() / 1000LL;
    const int64_t interval = (int64_t)s_refresh_interval_ms;
    if (now_ms - s_last_refresh_ok_ms < interval) {
        return false;
    }
    /* 数据已过期，但距上次真实尝试不足一周期时仍等待（失败退避）。 */
    return (now_ms - s_last_refresh_try_ms) >= interval;
}

bool espaperplay_weather_wait_refresh_done(uint32_t timeout_ms) {
    EventGroupHandle_t done = s_refresh_done;
    if (done == NULL || s_task == NULL) {
        return true; /* 任务未运行：无在途刷新可等 */
    }
    EventBits_t bits = xEventGroupWaitBits(done, BIT(0), pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT(0)) != 0;
}
