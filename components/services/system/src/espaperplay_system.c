/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "espaperplay_system.h"

static const char *TAG = "ESPaperPlay_SYSTEM";

/* NVS 键名。 */
#define NVS_KEY_WIFI_MODE "wifi_mode"
#define NVS_KEY_STA_SSID "sta_ssid"
#define NVS_KEY_STA_PASS "sta_pass"
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_AP_PASS "ap_pass"
#define NVS_KEY_EPD_IDLE_MS "epd_idle_ms"
#define NVS_KEY_GUI_FORCE_AFTER "gui_force_after"
#define NVS_KEY_WEATHER_KEY "weather_key"
#define NVS_KEY_WEATHER_LOC "weather_loc"
#define NVS_KEY_WEATHER_HOST "weather_host"
#define NVS_KEY_BOOT_LP_ACTION "boot_lp_action"
#define NVS_KEY_BOOT_LP_TIME_MS "boot_lp_time_ms"

/** 屏幕空闲睡眠超时上限（毫秒，24 小时）。 */
#define ESPAPERPLAY_SYSTEM_EPD_IDLE_TIMEOUT_MAX_MS 86400000u

/** 连续局刷后强制全刷阈值上限。 */
#define ESPAPERPLAY_SYSTEM_GUI_FULL_FORCE_AFTER_MAX 255u

/* 内存中的配置缓存，初始化为出厂默认值。 */
static espaperplay_system_config_t s_config = {
    .wifi_mode = ESPAPERPLAY_SYSTEM_DEFAULT_WIFI_MODE,
    .sta_ssid = ESPAPERPLAY_SYSTEM_DEFAULT_STA_SSID,
    .sta_password = ESPAPERPLAY_SYSTEM_DEFAULT_STA_PASS,
    .ap_ssid = ESPAPERPLAY_SYSTEM_DEFAULT_AP_SSID,
    .ap_password = ESPAPERPLAY_SYSTEM_DEFAULT_AP_PASS,
    .epd_idle_sleep_timeout_ms = ESPAPERPLAY_SYSTEM_DEFAULT_EPD_IDLE_SLEEP_TIMEOUT_MS,
    .gui_full_force_after = ESPAPERPLAY_SYSTEM_DEFAULT_GUI_FULL_FORCE_AFTER,
    .boot_long_press_action = ESPAPERPLAY_SYSTEM_DEFAULT_BOOT_LONG_PRESS_ACTION,
    .boot_long_press_time_ms = ESPAPERPLAY_SYSTEM_DEFAULT_BOOT_LONG_PRESS_TIME_MS,
    .weather_api_key = ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_API_KEY,
    .weather_location = ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_LOCATION,
    .weather_api_host = ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_API_HOST,
};

static bool s_initialized = false;

/* ------------------------------------------------------------------ */
/* NVS 底层辅助函数                                                     */
/* ------------------------------------------------------------------ */

static esp_err_t save_u8_field(const char *key, uint8_t value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPAPERPLAY_SYSTEM_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t save_u32_field(const char *key, uint32_t value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPAPERPLAY_SYSTEM_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t save_str_field(const char *key, const char *value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPAPERPLAY_SYSTEM_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/**
 * @brief 读取字符串字段；缺失或出错时回填默认值并标记 missing。
 */
static void load_str_field(nvs_handle_t handle, const char *key, char *dst, size_t dst_size,
                           const char *def, bool *missing) {
    size_t len = dst_size;
    esp_err_t err = nvs_get_str(handle, key, dst, &len);
    if (err == ESP_OK) {
        return;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read '%s': %s", key, esp_err_to_name(err));
    }
    *missing = true;
    strlcpy(dst, def, dst_size);
}

/** 将缓存中的全部配置写回 NVS。 */
static esp_err_t system_save_all(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPAPERPLAY_SYSTEM_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(handle, NVS_KEY_WIFI_MODE, (uint8_t)s_config.wifi_mode);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_STA_SSID, s_config.sta_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_STA_PASS, s_config.sta_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_AP_SSID, s_config.ap_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_AP_PASS, s_config.ap_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, NVS_KEY_EPD_IDLE_MS, s_config.epd_idle_sleep_timeout_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, NVS_KEY_GUI_FORCE_AFTER, s_config.gui_full_force_after);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, NVS_KEY_BOOT_LP_ACTION, (uint8_t)s_config.boot_long_press_action);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, NVS_KEY_BOOT_LP_TIME_MS, s_config.boot_long_press_time_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_WEATHER_KEY, s_config.weather_api_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_WEATHER_LOC, s_config.weather_location);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_WEATHER_HOST, s_config.weather_api_host);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/** 从 NVS 加载配置；缺失 / 非法的字段回填默认值。 */
static esp_err_t system_load(void) {
    nvs_handle_t handle;
    /* 用读写模式打开：首次上电时命名空间尚不存在，只读打开会返回
     * ESP_ERR_NVS_NOT_FOUND；读写模式会自动创建命名空间。 */
    esp_err_t err = nvs_open(ESPAPERPLAY_SYSTEM_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    bool missing = false;

    uint8_t mode = 0;
    err = nvs_get_u8(handle, NVS_KEY_WIFI_MODE, &mode);
    if (err == ESP_OK && mode < ESPAPERPLAY_WIFI_MODE_MAX) {
        s_config.wifi_mode = (espaperplay_wifi_mode_t)mode;
    } else {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to read '%s': %s", NVS_KEY_WIFI_MODE, esp_err_to_name(err));
        }
        s_config.wifi_mode = ESPAPERPLAY_SYSTEM_DEFAULT_WIFI_MODE;
        missing = true;
    }

    load_str_field(handle, NVS_KEY_STA_SSID, s_config.sta_ssid, sizeof(s_config.sta_ssid),
                   ESPAPERPLAY_SYSTEM_DEFAULT_STA_SSID, &missing);
    load_str_field(handle, NVS_KEY_STA_PASS, s_config.sta_password, sizeof(s_config.sta_password),
                   ESPAPERPLAY_SYSTEM_DEFAULT_STA_PASS, &missing);
    load_str_field(handle, NVS_KEY_AP_SSID, s_config.ap_ssid, sizeof(s_config.ap_ssid),
                   ESPAPERPLAY_SYSTEM_DEFAULT_AP_SSID, &missing);
    load_str_field(handle, NVS_KEY_AP_PASS, s_config.ap_password, sizeof(s_config.ap_password),
                   ESPAPERPLAY_SYSTEM_DEFAULT_AP_PASS, &missing);

    uint32_t idle_ms = 0;
    err = nvs_get_u32(handle, NVS_KEY_EPD_IDLE_MS, &idle_ms);
    if (err == ESP_OK && idle_ms <= ESPAPERPLAY_SYSTEM_EPD_IDLE_TIMEOUT_MAX_MS) {
        s_config.epd_idle_sleep_timeout_ms = idle_ms;
    } else {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to read '%s': %s", NVS_KEY_EPD_IDLE_MS, esp_err_to_name(err));
        }
        s_config.epd_idle_sleep_timeout_ms = ESPAPERPLAY_SYSTEM_DEFAULT_EPD_IDLE_SLEEP_TIMEOUT_MS;
        missing = true;
    }

    uint32_t force_after = 0;
    err = nvs_get_u32(handle, NVS_KEY_GUI_FORCE_AFTER, &force_after);
    if (err == ESP_OK && force_after <= ESPAPERPLAY_SYSTEM_GUI_FULL_FORCE_AFTER_MAX) {
        s_config.gui_full_force_after = force_after;
    } else {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to read '%s': %s", NVS_KEY_GUI_FORCE_AFTER, esp_err_to_name(err));
        }
        s_config.gui_full_force_after = ESPAPERPLAY_SYSTEM_DEFAULT_GUI_FULL_FORCE_AFTER;
        missing = true;
    }

    uint8_t boot_lp_action = 0;
    err = nvs_get_u8(handle, NVS_KEY_BOOT_LP_ACTION, &boot_lp_action);
    if (err == ESP_OK && boot_lp_action < ESPAPERPLAY_BOOT_LONG_PRESS_MAX) {
        s_config.boot_long_press_action = (espaperplay_boot_long_press_action_t)boot_lp_action;
    } else {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to read '%s': %s", NVS_KEY_BOOT_LP_ACTION, esp_err_to_name(err));
        }
        s_config.boot_long_press_action = ESPAPERPLAY_SYSTEM_DEFAULT_BOOT_LONG_PRESS_ACTION;
        missing = true;
    }

    uint32_t boot_lp_time_ms = 0;
    err = nvs_get_u32(handle, NVS_KEY_BOOT_LP_TIME_MS, &boot_lp_time_ms);
    if (err == ESP_OK && boot_lp_time_ms >= ESPAPERPLAY_SYSTEM_BOOT_LONG_PRESS_TIME_MIN_MS &&
        boot_lp_time_ms <= ESPAPERPLAY_SYSTEM_BOOT_LONG_PRESS_TIME_MAX_MS) {
        s_config.boot_long_press_time_ms = boot_lp_time_ms;
    } else {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to read '%s': %s", NVS_KEY_BOOT_LP_TIME_MS, esp_err_to_name(err));
        }
        s_config.boot_long_press_time_ms = ESPAPERPLAY_SYSTEM_DEFAULT_BOOT_LONG_PRESS_TIME_MS;
        missing = true;
    }

    load_str_field(handle, NVS_KEY_WEATHER_KEY, s_config.weather_api_key,
                   sizeof(s_config.weather_api_key), ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_API_KEY,
                   &missing);
    load_str_field(handle, NVS_KEY_WEATHER_LOC, s_config.weather_location,
                   sizeof(s_config.weather_location), ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_LOCATION,
                   &missing);
    load_str_field(handle, NVS_KEY_WEATHER_HOST, s_config.weather_api_host,
                   sizeof(s_config.weather_api_host), ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_API_HOST,
                   &missing);

    nvs_close(handle);

    if (missing) {
        ESP_LOGW(TAG, "Some config fields missing, writing defaults");
        return system_save_all();
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                            */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_system_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "System config already initialized");
        return ESP_OK;
    }

    /* 初始化 NVS 分区；分区满或格式版本变化时先擦除重建。 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = system_load();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load system config: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "System config loaded: mode=%s, STA(ssid=%s), AP(ssid=%s)",
             s_config.wifi_mode == ESPAPERPLAY_WIFI_MODE_AP ? "AP" : "STA", s_config.sta_ssid,
             s_config.ap_ssid);
    return ESP_OK;
}

const espaperplay_system_config_t *espaperplay_system_get_config(void) { return &s_config; }

esp_err_t espaperplay_system_set_wifi_mode(espaperplay_wifi_mode_t mode) {
    if (mode >= ESPAPERPLAY_WIFI_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config.wifi_mode = mode;
    return save_u8_field(NVS_KEY_WIFI_MODE, (uint8_t)mode);
}

esp_err_t espaperplay_system_set_sta_credentials(const char *ssid, const char *password) {
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= ESPAPERPLAY_SYSTEM_SSID_MAX_LEN ||
        strlen(password) >= ESPAPERPLAY_SYSTEM_PASS_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(s_config.sta_ssid, ssid, sizeof(s_config.sta_ssid));
    strlcpy(s_config.sta_password, password, sizeof(s_config.sta_password));

    esp_err_t err = save_str_field(NVS_KEY_STA_SSID, s_config.sta_ssid);
    if (err != ESP_OK) {
        return err;
    }
    return save_str_field(NVS_KEY_STA_PASS, s_config.sta_password);
}

esp_err_t espaperplay_system_set_ap_credentials(const char *ssid, const char *password) {
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= ESPAPERPLAY_SYSTEM_SSID_MAX_LEN ||
        strlen(password) >= ESPAPERPLAY_SYSTEM_PASS_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(s_config.ap_ssid, ssid, sizeof(s_config.ap_ssid));
    strlcpy(s_config.ap_password, password, sizeof(s_config.ap_password));

    esp_err_t err = save_str_field(NVS_KEY_AP_SSID, s_config.ap_ssid);
    if (err != ESP_OK) {
        return err;
    }
    return save_str_field(NVS_KEY_AP_PASS, s_config.ap_password);
}

esp_err_t espaperplay_system_set_epd_idle_sleep_timeout_ms(uint32_t timeout_ms) {
    if (timeout_ms > ESPAPERPLAY_SYSTEM_EPD_IDLE_TIMEOUT_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config.epd_idle_sleep_timeout_ms = timeout_ms;
    return save_u32_field(NVS_KEY_EPD_IDLE_MS, timeout_ms);
}

esp_err_t espaperplay_system_set_gui_full_force_after(uint32_t count) {
    if (count > ESPAPERPLAY_SYSTEM_GUI_FULL_FORCE_AFTER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config.gui_full_force_after = count;
    return save_u32_field(NVS_KEY_GUI_FORCE_AFTER, count);
}

esp_err_t espaperplay_system_set_boot_long_press_action(espaperplay_boot_long_press_action_t action) {
    if (action >= ESPAPERPLAY_BOOT_LONG_PRESS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (action != s_config.boot_long_press_action) {
        ESP_LOGI(TAG, "BOOT long-press action: %s",
                 action == ESPAPERPLAY_BOOT_LONG_PRESS_FULL_REFRESH
                     ? "full_refresh"
                     : (action == ESPAPERPLAY_BOOT_LONG_PRESS_BACK ? "back" : "none"));
        s_config.boot_long_press_action = action;
    }
    return save_u8_field(NVS_KEY_BOOT_LP_ACTION, (uint8_t)action);
}

espaperplay_boot_long_press_action_t espaperplay_system_get_boot_long_press_action(void) {
    return s_config.boot_long_press_action;
}

esp_err_t espaperplay_system_set_boot_long_press_time_ms(uint32_t time_ms) {
    if (time_ms < ESPAPERPLAY_SYSTEM_BOOT_LONG_PRESS_TIME_MIN_MS ||
        time_ms > ESPAPERPLAY_SYSTEM_BOOT_LONG_PRESS_TIME_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (time_ms != s_config.boot_long_press_time_ms) {
        ESP_LOGI(TAG, "BOOT long-press time: %u ms", (unsigned)time_ms);
        s_config.boot_long_press_time_ms = time_ms;
    }
    return save_u32_field(NVS_KEY_BOOT_LP_TIME_MS, time_ms);
}

uint32_t espaperplay_system_get_boot_long_press_time_ms(void) {
    return s_config.boot_long_press_time_ms;
}

esp_err_t espaperplay_system_set_weather_api_key(const char *key) {
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(key) >= ESPAPERPLAY_SYSTEM_WEATHER_KEY_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(s_config.weather_api_key, key, sizeof(s_config.weather_api_key));
    return save_str_field(NVS_KEY_WEATHER_KEY, s_config.weather_api_key);
}

esp_err_t espaperplay_system_set_weather_location(const char *location) {
    if (location == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(location) >= ESPAPERPLAY_SYSTEM_WEATHER_LOC_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(s_config.weather_location, location, sizeof(s_config.weather_location));
    return save_str_field(NVS_KEY_WEATHER_LOC, s_config.weather_location);
}

esp_err_t espaperplay_system_set_weather_api_host(const char *host) {
    if (host == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(host) >= ESPAPERPLAY_SYSTEM_WEATHER_HOST_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(s_config.weather_api_host, host, sizeof(s_config.weather_api_host));
    return save_str_field(NVS_KEY_WEATHER_HOST, s_config.weather_api_host);
}

esp_err_t espaperplay_system_reset_defaults(void) {
    s_config.wifi_mode = ESPAPERPLAY_SYSTEM_DEFAULT_WIFI_MODE;
    strlcpy(s_config.sta_ssid, ESPAPERPLAY_SYSTEM_DEFAULT_STA_SSID, sizeof(s_config.sta_ssid));
    strlcpy(s_config.sta_password, ESPAPERPLAY_SYSTEM_DEFAULT_STA_PASS,
            sizeof(s_config.sta_password));
    strlcpy(s_config.ap_ssid, ESPAPERPLAY_SYSTEM_DEFAULT_AP_SSID, sizeof(s_config.ap_ssid));
    strlcpy(s_config.ap_password, ESPAPERPLAY_SYSTEM_DEFAULT_AP_PASS, sizeof(s_config.ap_password));
    s_config.epd_idle_sleep_timeout_ms = ESPAPERPLAY_SYSTEM_DEFAULT_EPD_IDLE_SLEEP_TIMEOUT_MS;
    s_config.gui_full_force_after = ESPAPERPLAY_SYSTEM_DEFAULT_GUI_FULL_FORCE_AFTER;
    s_config.boot_long_press_action = ESPAPERPLAY_SYSTEM_DEFAULT_BOOT_LONG_PRESS_ACTION;
    s_config.boot_long_press_time_ms = ESPAPERPLAY_SYSTEM_DEFAULT_BOOT_LONG_PRESS_TIME_MS;
    strlcpy(s_config.weather_api_key, ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_API_KEY,
            sizeof(s_config.weather_api_key));
    strlcpy(s_config.weather_location, ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_LOCATION,
            sizeof(s_config.weather_location));
    strlcpy(s_config.weather_api_host, ESPAPERPLAY_SYSTEM_DEFAULT_WEATHER_API_HOST,
            sizeof(s_config.weather_api_host));
    return system_save_all();
}
