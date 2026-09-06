/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"

#include "espaperplay_clock.h"

static const char *TAG = "ESPaperPlay_CLOCK";

/** NVS 命名空间。 */
#define ESPAPERPLAY_CLOCK_NVS_NAMESPACE "clock"
/** NVS 中保存时区名称的键。 */
#define ESPAPERPLAY_CLOCK_NVS_KEY_TZ "tz"
/** NVS 中保存漂移标定模型的键。 */
#define ESPAPERPLAY_CLOCK_NVS_KEY_DRIFT "drift_ppm"
#define ESPAPERPLAY_CLOCK_NVS_KEY_HAVE "have_drift"
#define ESPAPERPLAY_CLOCK_NVS_KEY_REF_RTC "cal_ref_rtc"
#define ESPAPERPLAY_CLOCK_NVS_KEY_REF_TRUE "cal_ref_true"
#define ESPAPERPLAY_CLOCK_NVS_KEY_DONE "cal_done_rtc"
#define ESPAPERPLAY_CLOCK_NVS_KEY_COUNT "cal_count"

/** 默认 NTP 服务器列表（按优先级排序，需 CONFIG_LWIP_SNTP_MAX_SERVERS >= 3）。 */
static const char *s_ntp_servers[ESPAPERPLAY_CLOCK_NTP_SERVER_COUNT] = {
    "ntp.aliyun.com",
    "cn.pool.ntp.org",
    "pool.ntp.org",
};

/** 当前生效的时区名称（内存缓存）。 */
static char s_tz_name[ESPAPERPLAY_CLOCK_TZ_MAX_LEN] = ESPAPERPLAY_CLOCK_DEFAULT_TZ;

/** 当前已应用到 libc 的 POSIX TZ 字符串（默认 UTC）。 */
static char s_posix_tz[ESPAPERPLAY_CLOCK_TZ_MAX_LEN] = "UTC0";

/** SNTP 是否已初始化启动。 */
static bool s_sntp_started = false;

/** SNTP 同步完成事件位。 */
#define CLOCK_NTP_SYNCED_BIT BIT0
/** SNTP 同步事件组：每次成功对时由 sync_cb 置位，等待者 clear-on-exit 取走。
 * 刻意不使用 esp_netif_sntp 的 wait_for_sync 内部信号量：它随
 * esp_netif_sntp_deinit 销毁，而运行期 nettime 任务与 power 唤醒路径可能
 * 并发等待/重启对时——删除他人正阻塞其上的信号量会唤醒后访问已释放内存
 * （实测：唤醒重连后 spinlock count 断言崩溃）。事件组广播语义天然支持
 * 多任务并发等待同一次同步。 */
static EventGroupHandle_t s_ntp_evt = NULL;

/* ------------------------------------------------------------------ */
/* 软件 RTC 漂移标定（补偿 INT_RC 慢时钟误差，无需外部 32k 晶振）          */
/* ------------------------------------------------------------------ */
/* 关键事实（ESP-IDF v6 时间实现，已核实）：
 *   - 运行期 time(NULL) 由 esp_timer（XTAL 40MHz，精确）推进，误差≈0；
 *   - 浅睡眠期 esp_timer 暂停，唤醒时由 RTC(INT_RC) 测得的睡眠时长补回，
 *     故 time(NULL) 的误差【只在睡眠期间】按 RC 漂移率累积，运行期不漂移。
 * 因此不能用单一漂移率均匀校正全部流逝量（会过度校正运行期、不足校正睡眠
 * 期）。正确模型：误差 = e * 真实睡眠时长，仅对睡眠部分补偿。
 *
 * 设 e = RC 分数漂移率（每真实睡眠秒引入 e 秒误差，+快/-慢），
 * 某次睡眠的 RC 测得时长 S_meas = S_true*(1+e)，time(NULL) 在该次睡眠中
 * 多走 e*S_true 秒。累计测得睡眠 S_meas_acc 后，校正：
 *   corrected = raw - e/(1+e) * S_meas_acc
 * e 通过 NTP 对时测量：error = raw_before - T_true ≈ e * S_true_acc
 *   ≈ e * S_meas_acc（e 很小），故 e ≈ error / S_meas_acc。
 * 仅当累计睡眠足够大（>MIN_SLEEP_US）才接受一次测量，避免运行期为主时
 * 噪声过大。 */
#define ESPAPERPLAY_CLOCK_LEARNING_INTERVAL_S 600 /* 学习期：10 分钟 */
#define ESPAPERPLAY_CLOCK_STEADY_INTERVAL_S 7200  /* 稳定期：2 小时 */
#define ESPAPERPLAY_CLOCK_LEARNING_SAMPLES 3      /* 学习期采样数，达到即毕业 */
#define ESPAPERPLAY_CLOCK_MAX_DRIFT_PPM 20000     /* 漂移上限 ±2%，超出视为 NTP 异常 */
#define ESPAPERPLAY_CLOCK_MIN_SLEEP_US 60000000LL /* 单次测量所需最小累计睡眠（60s） */

static int32_t s_drift_ppm = 0;                /*!< RC 分数漂移率 e*1e6（每睡眠秒，+快/-慢） */
static bool s_have_drift = false;              /*!< 是否已测得有效漂移率 */
static int64_t s_cal_ref_rtc = 0;              /*!< 参考点：上次对时时刻的 raw 读数（仅日志） */
static int64_t s_cal_ref_true = 0;             /*!< 参考点：上次对时时刻的真实时间（仅日志） */
static int64_t s_sleep_measured = 0;           /*!< 自上次对时累计的 RC 测得睡眠时长（微秒） */
static int64_t s_cal_done_rtc = 0;             /*!< 上次标定完成时刻（墙钟，用于判断到期） */
static uint8_t s_cal_count = 0;                /*!< 已完成的标定采样数 */
static SemaphoreHandle_t s_drift_mutex = NULL; /*!< 保护漂移模型（UI 读 / 标定写） */

/* 前向声明：漂移模型辅助函数（定义见文件后部）。 */
static void clock_lock(void);
static void clock_unlock(void);
static void clock_load_drift(void);
static void clock_persist_drift(void);

/* ------------------------------------------------------------------ */
/* 时区                                                                */
/* ------------------------------------------------------------------ */

/**
 * IANA 时区名 -> POSIX TZ 字符串映射表。
 *
 * ESP-IDF 的 libc（newlib / picolibc）tzset() 只识别 POSIX 格式的 TZ 字符串
 * （如 "CST-8"），不支持 IANA 时区名（如 "Asia/Shanghai"）——传 IANA 名会
 * 解析失败并静默回退 UTC。因此 geoip 返回的 IANA 时区必须先翻译成 POSIX
 * 格式再应用。未收录的时区保持当前时区并告警（见 clock_apply_timezone）。
 *
 * POSIX TZ 语法：std offset [dst [offset2] [,start[/time],end[/time]]]，
 * 其中 offset 为「本地时间加该值得到 UTC」，符号与常识相反：西正东负，
 * 故 UTC+8（中国）写作 "CST-8"。
 */
typedef struct {
    const char *iana;  /*!< IANA 时区名（geoip 返回 / NVS 持久化的形式） */
    const char *posix; /*!< 对应的 POSIX TZ 字符串（tzset() 实际使用的形式） */
} clock_tz_map_entry_t;

static const clock_tz_map_entry_t s_tz_map[] = {
    /* ---- 固定偏移（无夏令时） ---- */
    {"UTC", "UTC0"},
    {"Etc/UTC", "UTC0"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Taipei", "CST-8"},
    {"Asia/Macau", "CST-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Seoul", "KST-9"},
    {"Asia/Singapore", "SGT-8"},
    {"Asia/Kuala_Lumpur", "MYT-8"},
    {"Asia/Bangkok", "ICT-7"},
    {"Asia/Ho_Chi_Minh", "ICT-7"},
    {"Asia/Jakarta", "WIB-7"},
    {"Asia/Manila", "PHT-8"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Dhaka", "BST-6"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Dubai", "GST-4"},
    {"Asia/Riyadh", "AST-3"},
    {"Asia/Baghdad", "AST-3"},
    {"Asia/Tehran", "IST-3:30"},
    {"Asia/Yangon", "MMT-6:30"},
    {"Asia/Jerusalem", "IST-2"},
    {"Europe/Moscow", "MSK-3"},
    {"Europe/Istanbul", "TRT-3"},
    {"Europe/Kyiv", "EET-2"},
    {"Europe/Athens", "EET-2"},
    {"Europe/Helsinki", "EET-2"},
    {"Europe/Bucharest", "EET-2"},
    {"Europe/Riga", "EET-2"},
    {"Europe/Vilnius", "EET-2"},
    {"Europe/Tallinn", "EET-2"},
    {"Africa/Cairo", "EET-2"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Lagos", "WAT-1"},
    {"Africa/Nairobi", "EAT-3"},
    {"Australia/Perth", "AWST-8"},
    {"Australia/Brisbane", "AEST-10"},
    {"Pacific/Honolulu", "HST10"},
    {"Pacific/Guam", "ChST-10"},
    {"Pacific/Fiji", "FJT-12"},
    {"America/Phoenix", "MST7"},
    {"America/Mexico_City", "CST6"},
    {"America/Puerto_Rico", "AST4"},
    {"America/Sao_Paulo", "BRT+3"},
    {"America/Argentina/Buenos_Aires", "ART+3"},
    {"America/Bogota", "COT+5"},
    {"America/Lima", "PET+5"},
    {"America/Caracas", "VET+4"},
    {"America/Santiago", "CLT+4"},
    /* ---- 北美（3 月第二个周日 / 11 月第一个周日，2:00 本地） ---- */
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Montreal", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Winnipeg", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Edmonton", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"},
    /* ---- 欧洲（欧盟统一规则：3 月最后周日 / 10 月最后周日） ---- */
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Belgrade", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    /* ---- 澳洲 / 新西兰 ---- */
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

/** IANA 时区名 -> POSIX TZ 字符串；未收录返回 NULL。 */
static const char *clock_iana_to_posix(const char *iana) {
    for (size_t i = 0; i < sizeof(s_tz_map) / sizeof(s_tz_map[0]); i++) {
        if (strcmp(s_tz_map[i].iana, iana) == 0) {
            return s_tz_map[i].posix;
        }
    }
    return NULL;
}

/** 应用时区到 libc（setenv("TZ") + tzset()，仅支持 POSIX 格式）。 */
static void clock_apply_timezone(const char *tz_name) {
    const char *posix = clock_iana_to_posix(tz_name);
    if (posix == NULL) {
        /* 未收录的时区：保持当前已生效的时区，避免误切到 UTC。 */
        ESP_LOGW(TAG, "IANA timezone \"%s\" has no POSIX mapping, keeping \"%s\"", tz_name,
                 s_posix_tz);
        return;
    }
    strlcpy(s_posix_tz, posix, sizeof(s_posix_tz));
    setenv("TZ", s_posix_tz, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set to \"%s\" (posix \"%s\")", tz_name, s_posix_tz);
}

/** 从 NVS 读取持久化的时区名称；不存在或读取失败返回 false。 */
static bool clock_load_timezone(char *tz_out, size_t tz_out_len) {
    nvs_handle_t handle;
    if (nvs_open(ESPAPERPLAY_CLOCK_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(handle, ESPAPERPLAY_CLOCK_NVS_KEY_TZ, tz_out, &tz_out_len) == ESP_OK;
    nvs_close(handle);
    return ok;
}

/** 把时区名称持久化到 NVS（失败仅告警，不影响本次生效）。 */
static void clock_store_timezone(const char *tz_name) {
    nvs_handle_t handle;
    if (nvs_open(ESPAPERPLAY_CLOCK_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "failed to open nvs namespace \"%s\"", ESPAPERPLAY_CLOCK_NVS_NAMESPACE);
        return;
    }
    esp_err_t err = nvs_set_str(handle, ESPAPERPLAY_CLOCK_NVS_KEY_TZ, tz_name);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist timezone \"%s\": %s", tz_name, esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------ */
/* NTP                                                                */
/* ------------------------------------------------------------------ */

/** SNTP 同步回调：每次对时（初始/周期/强制）后更新校正基准。
 * 系统时间由 lwIP 的 sntp_sync_time 独立设置，本回调仅作附加通知，
 * 确保校正基准始终跟踪最新真实时间，避免与软件漂移补偿重复校正。 */
static void clock_sntp_sync_cb(struct timeval *tv) {
    (void)tv;
    if (s_ntp_evt != NULL) {
        xEventGroupSetBits(s_ntp_evt, CLOCK_NTP_SYNCED_BIT);
    }
    espaperplay_clock_mark_synced();
}

/** 初始化并启动 SNTP 客户端（内部复用，不检查 s_sntp_started）。 */
static esp_err_t clock_sntp_init_start(void) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        ESPAPERPLAY_CLOCK_NTP_SERVER_COUNT,
        ESP_SNTP_SERVER_LIST(s_ntp_servers[0], s_ntp_servers[1], s_ntp_servers[2]));
    config.sync_cb = clock_sntp_sync_cb;
    /* 等待同步走组件自持事件组（见 s_ntp_evt 注释），不创建内部信号量。 */
    config.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to init sntp: %s", esp_err_to_name(err));
        return err;
    }
    if (s_ntp_evt != NULL) {
        /* 清除历史残留位，使本次启动后的首次同步才视为新鲜事件。 */
        xEventGroupClearBits(s_ntp_evt, CLOCK_NTP_SYNCED_BIT);
    }
    err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start sntp: %s", esp_err_to_name(err));
        esp_netif_sntp_deinit();
        return err;
    }
    s_sntp_started = true;
    ESP_LOGI(TAG, "sntp started (servers: %s, %s, %s)", s_ntp_servers[0], s_ntp_servers[1],
             s_ntp_servers[2]);
    return ESP_OK;
}

esp_err_t espaperplay_clock_ntp_start(void) {
    if (s_sntp_started) {
        return ESP_OK;
    }
    return clock_sntp_init_start();
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_clock_init(void) {
    char tz[ESPAPERPLAY_CLOCK_TZ_MAX_LEN] = ESPAPERPLAY_CLOCK_DEFAULT_TZ;
    if (clock_load_timezone(tz, sizeof(tz))) {
        ESP_LOGI(TAG, "restored persisted timezone \"%s\"", tz);
    } else {
        ESP_LOGI(TAG, "no persisted timezone, using default \"%s\"", tz);
    }
    strlcpy(s_tz_name, tz, sizeof(s_tz_name));
    clock_apply_timezone(s_tz_name);

    s_drift_mutex = xSemaphoreCreateMutex();
    if (s_drift_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create drift mutex");
    }
    s_ntp_evt = xEventGroupCreate();
    if (s_ntp_evt == NULL) {
        ESP_LOGE(TAG, "failed to create ntp sync event group");
    }
    clock_load_drift();
    /* 重启后保留已测得的漂移率用于即时校正；累计睡眠与采样数清零，
     * 由下次对时重新累积（睡眠时长不跨重启持久化）。 */
    s_sleep_measured = 0;
    s_cal_count = 0;
    return ESP_OK;
}

esp_err_t espaperplay_clock_set_timezone(const char *tz_name) {
    const char *tz =
        (tz_name == NULL || tz_name[0] == '\0') ? ESPAPERPLAY_CLOCK_DEFAULT_TZ : tz_name;
    if (strlen(tz) >= sizeof(s_tz_name)) {
        ESP_LOGE(TAG, "timezone name too long: \"%s\"", tz);
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_tz_name, tz, sizeof(s_tz_name));
    clock_apply_timezone(s_tz_name);
    clock_store_timezone(s_tz_name);
    return ESP_OK;
}

esp_err_t espaperplay_clock_get_timezone(char *tz_out, size_t tz_out_len) {
    if (tz_out == NULL || tz_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(tz_out, s_tz_name, tz_out_len);
    return ESP_OK;
}

esp_err_t espaperplay_clock_ntp_wait_sync(uint32_t timeout_ms) {
    if (!s_sntp_started || s_ntp_evt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const EventBits_t bits =
        xEventGroupWaitBits(s_ntp_evt, CLOCK_NTP_SYNCED_BIT, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(timeout_ms));
    if (bits & CLOCK_NTP_SYNCED_BIT) {
        ESP_LOGI(TAG, "time synchronized via ntp");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "ntp sync timed out after %u ms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* 漂移标定模型                                                        */
/* ------------------------------------------------------------------ */

static void clock_lock(void) {
    if (s_drift_mutex) {
        xSemaphoreTake(s_drift_mutex, portMAX_DELAY);
    }
}

static void clock_unlock(void) {
    if (s_drift_mutex) {
        xSemaphoreGive(s_drift_mutex);
    }
}

/** 从 NVS 恢复漂移标定模型。 */
static void clock_load_drift(void) {
    nvs_handle_t h;
    if (nvs_open(ESPAPERPLAY_CLOCK_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v;
    if (nvs_get_i32(h, ESPAPERPLAY_CLOCK_NVS_KEY_DRIFT, &v) == ESP_OK) {
        s_drift_ppm = v;
    }
    uint8_t u;
    if (nvs_get_u8(h, ESPAPERPLAY_CLOCK_NVS_KEY_HAVE, &u) == ESP_OK) {
        s_have_drift = (u != 0);
    }
    int64_t i;
    if (nvs_get_i64(h, ESPAPERPLAY_CLOCK_NVS_KEY_REF_RTC, &i) == ESP_OK) {
        s_cal_ref_rtc = i;
    }
    if (nvs_get_i64(h, ESPAPERPLAY_CLOCK_NVS_KEY_REF_TRUE, &i) == ESP_OK) {
        s_cal_ref_true = i;
    }
    if (nvs_get_i64(h, ESPAPERPLAY_CLOCK_NVS_KEY_DONE, &i) == ESP_OK) {
        s_cal_done_rtc = i;
    }
    if (nvs_get_u8(h, ESPAPERPLAY_CLOCK_NVS_KEY_COUNT, &u) == ESP_OK) {
        s_cal_count = u;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "drift model loaded: drift=%ld ppm have=%d samples=%u", (long)s_drift_ppm,
             s_have_drift, s_cal_count);
}

/** 持久化漂移标定模型到 NVS。 */
static void clock_persist_drift(void) {
    nvs_handle_t h;
    if (nvs_open(ESPAPERPLAY_CLOCK_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, ESPAPERPLAY_CLOCK_NVS_KEY_DRIFT, s_drift_ppm);
    nvs_set_u8(h, ESPAPERPLAY_CLOCK_NVS_KEY_HAVE, s_have_drift ? 1 : 0);
    nvs_set_i64(h, ESPAPERPLAY_CLOCK_NVS_KEY_REF_RTC, s_cal_ref_rtc);
    nvs_set_i64(h, ESPAPERPLAY_CLOCK_NVS_KEY_REF_TRUE, s_cal_ref_true);
    nvs_set_i64(h, ESPAPERPLAY_CLOCK_NVS_KEY_DONE, s_cal_done_rtc);
    nvs_set_u8(h, ESPAPERPLAY_CLOCK_NVS_KEY_COUNT, s_cal_count);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t espaperplay_clock_mark_synced(void) {
    int64_t t = (int64_t)time(NULL);
    if (t <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    clock_lock();
    s_cal_ref_rtc = t;
    s_cal_ref_true = t;
    s_sleep_measured = 0; /* 重置累计睡眠，作为新测量窗口起点 */
    s_cal_done_rtc = t;   /* 刷新标定到期计时，使首次标定等满一个学习间隔 */
    clock_persist_drift();
    clock_unlock();
    return ESP_OK;
}

esp_err_t espaperplay_clock_resync_now(uint32_t timeout_ms) {
    esp_err_t err;
    if (!s_sntp_started) {
        err = clock_sntp_init_start();
        if (err != ESP_OK) {
            return err;
        }
    } else {
        /* 绝不在运行期 esp_netif_sntp_deinit：nettime 任务可能正阻塞在对时
         * 等待上，deinit 会销毁它脚下的句柄（历史信号量路径已整体弃用）。
         * 复位事件位并重启轮询（内部 sntp_stop+sntp_init，服务器配置保留），
         * 立即发起一轮全新同步采样，供漂移标定取得新鲜读数。 */
        if (s_ntp_evt != NULL) {
            xEventGroupClearBits(s_ntp_evt, CLOCK_NTP_SYNCED_BIT);
        }
        err = esp_netif_sntp_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to restart sntp: %s", esp_err_to_name(err));
            /* 继续等待：旧轮询可能仍在推进，超时由下方统一处理。 */
        }
    }
    err = espaperplay_clock_ntp_wait_sync(timeout_ms);
    if (err == ESP_OK) {
        espaperplay_clock_mark_synced();
    } else if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "ntp resync timed out after %u ms", timeout_ms);
    } else {
        ESP_LOGW(TAG, "ntp resync failed: %s", esp_err_to_name(err));
    }
    return err;
}

void espaperplay_clock_account_sleep(uint64_t sleep_us) {
    if (sleep_us == 0) {
        return;
    }
    clock_lock();
    s_sleep_measured += (int64_t)sleep_us;
    clock_unlock();
}

esp_err_t espaperplay_clock_calibrate(uint32_t timeout_ms) {
    /* 先快照累计睡眠与对时前读数（resync 会重置 s_sleep_measured）。 */
    clock_lock();
    int64_t sleep_snap = s_sleep_measured;
    clock_unlock();
    int64_t raw_before = (int64_t)time(NULL);

    /* 累计睡眠不足：运行期为主，误差可忽略且测量噪声大，跳过本次对时，
     * 仅刷新到期计时，待后续睡眠累积足够再测。不重置睡眠累计，使其跨周期
     * 累积。 */
    if (sleep_snap < ESPAPERPLAY_CLOCK_MIN_SLEEP_US) {
        clock_lock();
        s_cal_done_rtc = (int64_t)time(NULL);
        clock_persist_drift();
        clock_unlock();
        ESP_LOGD(TAG, "calibration skipped: sleep %lld us < min %lld us", (long long)sleep_snap,
                 (long long)ESPAPERPLAY_CLOCK_MIN_SLEEP_US);
        return ESP_OK;
    }

    esp_err_t err = espaperplay_clock_resync_now(timeout_ms);
    int64_t T = (int64_t)time(NULL);
    if (err != ESP_OK) {
        return err; /* 对时失败：resync 内部未重置睡眠，下次重试 */
    }

    /* error = 对时前读数 - 真实时间 ≈ e * 真实睡眠时长 ≈ e * 测得睡眠时长
     * （e 很小）。测得睡眠用快照 sleep_snap（resync 已重置实时值）。 */
    int64_t error_us = (raw_before - T) * 1000000LL;
    double e = (double)error_us / (double)sleep_snap;
    int32_t ppm = (int32_t)(e * 1e6);
    bool valid =
        (ppm >= -ESPAPERPLAY_CLOCK_MAX_DRIFT_PPM && ppm <= ESPAPERPLAY_CLOCK_MAX_DRIFT_PPM);

    clock_lock();
    if (valid) {
        if (!s_have_drift) {
            s_drift_ppm = ppm;
        } else {
            s_drift_ppm = (int32_t)((double)s_drift_ppm * 0.5 + (double)ppm * 0.5);
        }
        s_have_drift = true;
    } else {
        ESP_LOGW(TAG, "measured drift %ld ppm out of range, ignored", (long)ppm);
    }
    /* resync 已重置 s_sleep_measured=0 与参考点；此处仅更新漂移率与采样数。 */
    s_cal_done_rtc = T;
    s_cal_count++;
    clock_persist_drift();
    clock_unlock();
    ESP_LOGI(TAG, "clock calibrated: drift=%ld ppm valid=%d samples=%u sleep=%lld us",
             (long)s_drift_ppm, valid, s_cal_count, (long long)sleep_snap);
    return ESP_OK;
}

bool espaperplay_clock_is_calibration_due(void) {
    int64_t now = (int64_t)time(NULL);
    if (s_cal_done_rtc <= 0) {
        return true; /* 从未标定 -> 立即开始学习 */
    }
    bool learning = (!s_have_drift) || (s_cal_count < ESPAPERPLAY_CLOCK_LEARNING_SAMPLES);
    int64_t interval =
        learning ? ESPAPERPLAY_CLOCK_LEARNING_INTERVAL_S : ESPAPERPLAY_CLOCK_STEADY_INTERVAL_S;
    return (now - s_cal_done_rtc) >= interval;
}

int32_t espaperplay_clock_get_drift_ppm(void) {
    int32_t v;
    clock_lock();
    v = s_drift_ppm;
    clock_unlock();
    return v;
}

esp_err_t espaperplay_clock_get_local_time(struct tm *local_time) {
    if (local_time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    time_t now = time(NULL);
    clock_lock();
    bool have = s_have_drift;
    int32_t ppm = s_drift_ppm;
    int64_t sleep_us = s_sleep_measured;
    clock_unlock();
    if (have) {
        /* 仅对睡眠部分补偿：误差 = e/(1+e) * 测得睡眠时长（运行期 XTAL 精确，
         * 不引入误差）。e = ppm/1e6。 */
        double e = (double)ppm / 1e6;
        int64_t sleep_err_s = (int64_t)((double)sleep_us * e / (1.0 + e) / 1e6);
        int64_t corrected = (int64_t)now - sleep_err_s;
        localtime_r(&corrected, local_time);
    } else {
        localtime_r(&now, local_time);
    }
    return ESP_OK;
}
