/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "espaperplay_wifi.h"

static const char *TAG = "ESPaperPlay_WIFI";

/* ------------------------------------------------------------------ */
/* 运行状态                                                             */
/* ------------------------------------------------------------------ */

static esp_netif_t *s_sta_netif = NULL; /*!< STA 网络接口 */
static esp_netif_t *s_ap_netif = NULL;  /*!< AP 网络接口 */
static esp_event_handler_instance_t s_wifi_event_instance = NULL;
static esp_event_handler_instance_t s_ip_event_instance = NULL;

static bool s_started = false;        /*!< WiFi 驱动是否已启动 */
static bool s_connected = false;      /*!< 网络是否可用（AP：热点已开；STA：已获取 IP） */
static bool s_auto_reconnect = false; /*!< STA 断线后是否自动重连 */
static bool s_sleep_suppress_reconnect =
    false; /*!< 睡眠期间抑制自动重连（主动断开后由电源管理控制） */
static espaperplay_wifi_mode_t s_mode = ESPAPERPLAY_WIFI_MODE_AP; /*!< 当前工作模式 */
static char s_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN] = "";         /*!< 当前 SSID */
static char s_ip[16] = "0.0.0.0";                                 /*!< 当前 IP */
static uint8_t s_sta_retry_count = 0;                             /*!< 已执行的自动重连次数 */
static bool s_boot_in_progress = false;       /*!< 是否处于系统启动阶段（允许回退 AP） */
static TimerHandle_t s_fallback_timer = NULL; /*!< 回退到 AP 的延时定时器 */

/* ------------------------------------------------------------------ */
/* WiFi 事件处理                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief 根据配置更新 AP 热点参数。
 *
 * 密码为空时按开放网络（WIFI_AUTH_OPEN）启动，否则使用 WPA2_PSK。
 * 密码非空但小于 8 位时 ESP-IDF 会拒绝（返回 ESP_ERR_WIFI_PASSWORD），
 * 此处交由上层日志反馈。
 */
static esp_err_t wifi_apply_ap_config(const espaperplay_system_config_t *cfg) {
    wifi_config_t wifi_cfg = {0};
    wifi_cfg.ap.ssid_len = 0;
    wifi_cfg.ap.channel = ESPAPERPLAY_WIFI_AP_CHANNEL;
    wifi_cfg.ap.max_connection = ESPAPERPLAY_WIFI_AP_MAX_CONNECTION;
    wifi_cfg.ap.ssid_hidden = 0;
    wifi_cfg.ap.authmode = (cfg->ap_password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    strlcpy((char *)wifi_cfg.ap.ssid, cfg->ap_ssid, sizeof(wifi_cfg.ap.ssid));
    strlcpy((char *)wifi_cfg.ap.password, cfg->ap_password, sizeof(wifi_cfg.ap.password));

    ESP_LOGI(TAG, "AP config: ssid=\"%s\", auth=%s, max_conn=%u", (char *)wifi_cfg.ap.ssid,
             wifi_cfg.ap.authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2_PSK",
             wifi_cfg.ap.max_connection);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }
    return esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
}

/**
 * @brief 根据配置更新 STA 站点参数。
 *
 * 密码为空时按开放网络连接（阈值 WIFI_AUTH_OPEN）；密码非空时携带密码连接，
 * 阈值设为 WIFI_AUTH_WPA_PSK（至少 WPA），以兼容 WPA / WPA2 / WPA3 以及
 * WPA/WPA2 混合热点。
 *
 * @note 密码非空时阈值不能保持 OPEN：WiFi 驱动检测到密码长度 ≥8 会把它自动
 * 抬升为 WPA2_PSK，导致按 WPA_PSK 上报的 WPA/WPA2 混合热点被过滤（找不到
 * AP）。显式设为 WPA_PSK 可绕过驱动的自动抬升。
 */
static esp_err_t wifi_apply_sta_config(const espaperplay_system_config_t *cfg) {
    wifi_config_t wifi_cfg = {0};
    wifi_cfg.sta.scan_method = WIFI_FAST_SCAN;
    wifi_cfg.sta.bssid_set = 0;
    wifi_cfg.sta.channel = 0;
    wifi_cfg.sta.threshold.rssi = -127;
    wifi_cfg.sta.threshold.authmode =
        (cfg->sta_password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;

    strlcpy((char *)wifi_cfg.sta.ssid, cfg->sta_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, cfg->sta_password, sizeof(wifi_cfg.sta.password));

    ESP_LOGI(TAG, "STA config: ssid=\"%s\", password=%s", (char *)wifi_cfg.sta.ssid,
             (cfg->sta_password[0] == '\0') ? "(none/open)" : "(set)");

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
}

/** 应用指定模式的配置；同步更新状态缓存（模式 / SSID）。 */
static esp_err_t wifi_apply_config(espaperplay_wifi_mode_t mode) {
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();

    esp_err_t err;
    switch (mode) {
    case ESPAPERPLAY_WIFI_MODE_AP:
        err = wifi_apply_ap_config(cfg);
        break;
    case ESPAPERPLAY_WIFI_MODE_STA:
        err = wifi_apply_sta_config(cfg);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set wifi config: %s", esp_err_to_name(err));
        return err;
    }

    s_mode = mode;
    strlcpy(s_ssid, (mode == ESPAPERPLAY_WIFI_MODE_AP) ? cfg->ap_ssid : cfg->sta_ssid,
            sizeof(s_ssid));
    return ESP_OK;
}

/** 查询 AP 接口的默认 IP（如 192.168.4.1）并写入状态缓存。 */
static void wifi_update_ap_ip(void) {
    esp_netif_ip_info_t ip_info;
    if (s_ap_netif != NULL && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGI(TAG, "AP IP: %s", s_ip);
    }
}

/**
 * @brief 回退到 AP 模式。
 *
 * 由回退定时器在系统启动阶段 STA 连接失败后调用：停止当前 STA，重新应用
 * 系统配置中的 AP 参数并启动热点。执行前会校验仍处于启动阶段且当前为 STA，
 * 避免定时器触发时与运行期操作冲突。
 */
static esp_err_t wifi_fallback_to_ap(void) {
    /* 定时器触发时状态可能已变化（如被手动 stop），做双重校验。 */
    if (!s_started || s_mode != ESPAPERPLAY_WIFI_MODE_STA) {
        ESP_LOGW(TAG, "AP fallback skipped, wifi state changed");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA connect failed during startup, falling back to AP mode");
    s_auto_reconnect = false;

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "esp_wifi_stop failed during fallback: %s", esp_err_to_name(err));
        return err;
    }
    s_started = false;
    s_connected = false;

    err = wifi_apply_config(ESPAPERPLAY_WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed during fallback: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "WiFi fell back to AP mode (ssid=\"%s\")", s_ssid);
    return ESP_OK;
}

/** 回退定时器回调：在定时器任务上下文中执行模式切换。 */
static void wifi_fallback_timer_cb(TimerHandle_t timer) {
    (void)timer;
    wifi_fallback_to_ap();
}

/** WiFi 事件处理器：维护连接 / 断线 / 自动重连逻辑。 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {
    (void)arg;
    (void)event_base;

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA started, connecting to \"%s\"...", s_ssid);
        s_sta_retry_count = 0;
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "STA connected to AP");
        s_sta_retry_count = 0;
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "STA disconnected, reason=%u", (unsigned)disc->reason);
        s_connected = false;
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));

        /* 睡眠期间主动断开：抑制自动重连，交由电源管理在唤醒后决定是否重连。 */
        if (s_sleep_suppress_reconnect) {
            ESP_LOGI(TAG, "STA disconnect suppressed for sleep (no auto-reconnect)");
            break;
        }

        /* 仅在主动运行状态下自动重连，避免 stop / 配置切换期间被事件触发重连。 */
        if (s_auto_reconnect && s_started) {
            if (s_sta_retry_count < ESPAPERPLAY_WIFI_STA_RECONNECT_RETRY_MAX) {
                s_sta_retry_count++;
                ESP_LOGI(TAG, "Auto reconnect attempt %u/%d", (unsigned)s_sta_retry_count,
                         ESPAPERPLAY_WIFI_STA_RECONNECT_RETRY_MAX);
                esp_wifi_connect();
            } else {
                if (s_boot_in_progress) {
                    /* 启动阶段连接失败：关闭自动重连，延时后由定时器回退到 AP。 */
                    s_boot_in_progress = false;
                    s_auto_reconnect = false;
                    ESP_LOGW(TAG, "Startup STA connect failed, scheduling AP fallback");
                    if (xTimerStart(s_fallback_timer, 0) != pdPASS) {
                        ESP_LOGE(TAG, "Failed to start AP fallback timer");
                    }
                } else {
                    ESP_LOGW(TAG, "Reconnect attempts exhausted, giving up until next start");
                    s_auto_reconnect = false;
                }
            }
        }
        break;
    }

    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "AP started, ssid=\"%s\"", s_ssid);
        s_connected = true;
        wifi_update_ap_ip();
        break;

    case WIFI_EVENT_AP_STOP:
        ESP_LOGI(TAG, "AP stopped");
        s_connected = false;
        break;

    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station connected: %02x:%02x:%02x:%02x:%02x:%02x", evt->mac[0], evt->mac[1],
                 evt->mac[2], evt->mac[3], evt->mac[4], evt->mac[5]);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station disconnected: %02x:%02x:%02x:%02x:%02x:%02x", evt->mac[0],
                 evt->mac[1], evt->mac[2], evt->mac[3], evt->mac[4], evt->mac[5]);
        break;
    }

    default:
        break;
    }
}

/** IP 事件处理器：STA 模式获取 / 丢失 IP 时更新状态。 */
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                             void *event_data) {
    (void)arg;
    (void)event_base;

    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_connected = true;
        /* 启动阶段成功联网，关闭启动期回退。 */
        s_boot_in_progress = false;
        ESP_LOGI(TAG, "STA got IP: %s", s_ip);
        break;
    }
    case IP_EVENT_STA_LOST_IP:
        ESP_LOGW(TAG, "STA lost IP");
        s_connected = false;
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                            */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_wifi_init(void) {
    static bool s_initialized = false;
    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi service already initialized");
        return ESP_OK;
    }

    /* 初始化网络接口与事件循环（系统配置服务已初始化 NVS）。 */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi netif");
        return ESP_FAIL;
    }

    /* 设置 STA 主机名，便于在路由器设备列表中识别本机。 */
    err = esp_netif_set_hostname(s_sta_netif, "ESPaperPlay");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler,
                                              NULL, &s_wifi_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register wifi event handler: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL,
                                              &s_ip_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ip event handler: %s", esp_err_to_name(err));
        return err;
    }

    /* 创建启动期 AP 回退使用的延时定时器。 */
    s_fallback_timer =
        xTimerCreate("wifi_fallback", pdMS_TO_TICKS(ESPAPERPLAY_WIFI_FALLBACK_DELAY_MS), pdFALSE,
                     NULL, wifi_fallback_timer_cb);
    if (s_fallback_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create AP fallback timer");
        return ESP_FAIL;
    }

    s_initialized = true;

    /* 标记处于系统启动阶段：仅此阶段允许 STA 失败后回退到 AP。 */
    s_boot_in_progress = true;
    return espaperplay_wifi_start();
}

esp_err_t espaperplay_wifi_start(void) {
    /* 若 WiFi 已处于启动状态（例如运行期切换模式 / 重新应用配置），
     * esp_wifi_set_mode / set_config 要求驱动处于停止态，故先停止。 */
    if (s_started) {
        s_auto_reconnect = false;
        esp_wifi_stop();
        s_started = false;
        s_connected = false;
    }

    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();

    esp_err_t err = wifi_apply_config(cfg->wifi_mode);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    s_auto_reconnect = (s_mode == ESPAPERPLAY_WIFI_MODE_STA);
    ESP_LOGI(TAG, "WiFi started in %s mode (ssid=\"%s\")",
             s_mode == ESPAPERPLAY_WIFI_MODE_AP ? "AP" : "STA", s_ssid);
    return ESP_OK;
}

esp_err_t espaperplay_wifi_stop(void) {
    /* 先关闭自动重连与启动期回退，防止 stop 触发的断线事件引起重连 / 回退。 */
    s_auto_reconnect = false;
    s_boot_in_progress = false;

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
        return err;
    }

    s_started = false;
    s_connected = false;
    strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
    ESP_LOGI(TAG, "WiFi stopped");
    return ESP_OK;
}

/**
 * @brief 进入浅睡眠前主动断开 STA 并抑制自动重连。
 *
 * 手动 esp_light_sleep_start() 期间 WiFi modem 完全断电，无法按 listen
 * interval 收信标，AP 侧会因站点长时间无活动而解关联，唤醒后触发
 * BEACON_TIMEOUT 被动断开 + 自动重连（约 2.5s 活跃爆发，且对仅刷新时钟
 * 的定时器唤醒毫无必要）。本函数在睡眠前显式断开并置位抑制标志，使断开
 * 时机可控、日志干净；唤醒后由 espaperplay_wifi_resume_after_wake() 决定
 * 是否重连。
 *
 * 仅 STA 模式生效；AP 模式（热点）保持运行不处理。
 *
 * @return 成功返回 ESP_OK（已断开或本就未连接），否则返回错误码。
 */
esp_err_t espaperplay_wifi_suspend_for_sleep(void) {
    if (s_mode != ESPAPERPLAY_WIFI_MODE_STA || !s_started) {
        return ESP_OK; /* AP 模式或未启动：无需处理 */
    }
    s_sleep_suppress_reconnect = true;
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        /* 未连接时断开会返回 ESP_ERR_WIFI_NOT_CONNECT，属正常情况，忽略。 */
        ESP_LOGD(TAG, "esp_wifi_disconnect for sleep: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "WiFi suspended for sleep (STA disconnected, reconnect suppressed)");
    return ESP_OK;
}

/**
 * @brief 浅睡眠唤醒后恢复 STA 关联策略。
 *
 * @param reconnect true=清除抑制标志并立即重连（用户操作唤醒，需恢复网络）；
 *                  false=保持断开（定时器唤醒仅刷新时钟，无需网络）。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_wifi_resume_after_wake(bool reconnect) {
    if (s_mode != ESPAPERPLAY_WIFI_MODE_STA || !s_started) {
        return ESP_OK; /* AP 模式或未启动：无需处理 */
    }
    if (reconnect) {
        s_sleep_suppress_reconnect = false;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            /* 已连接时重连可能返回 ESP_ERR_WIFI_STATE 等，属正常情况，忽略。 */
            ESP_LOGD(TAG, "esp_wifi_connect after wake: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "WiFi resumed after wake (reconnect requested)");
    } else {
        /* 保持断开：抑制标志维持，避免后续被动断开事件触发重连。 */
        ESP_LOGI(TAG, "WiFi kept disconnected after timer wake");
    }
    return ESP_OK;
}

esp_err_t espaperplay_wifi_get_status(espaperplay_wifi_status_t *status) {
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    status->started = s_started;
    status->connected = s_connected;
    status->mode = s_mode;
    strlcpy(status->ssid, s_ssid, sizeof(status->ssid));
    strlcpy(status->ip, s_ip, sizeof(status->ip));
    return ESP_OK;
}

esp_err_t espaperplay_wifi_get_rssi(int *out_rssi) {
    if (out_rssi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 仅 STA 已连接时才有 RSSI（AP 模式无意义）。 */
    if (!s_started || s_mode != ESPAPERPLAY_WIFI_MODE_STA || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_rssi = ap.rssi;
    return ESP_OK;
}

bool espaperplay_wifi_is_connected(void) { return s_connected; }
