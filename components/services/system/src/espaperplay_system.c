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
#define NVS_KEY_STA_SSID  "sta_ssid"
#define NVS_KEY_STA_PASS  "sta_pass"
#define NVS_KEY_AP_SSID   "ap_ssid"
#define NVS_KEY_AP_PASS   "ap_pass"

/* 内存中的配置缓存，初始化为出厂默认值。 */
static espaperplay_system_config_t s_config = {
    .wifi_mode = ESPAPERPLAY_SYSTEM_DEFAULT_WIFI_MODE,
    .sta_ssid = ESPAPERPLAY_SYSTEM_DEFAULT_STA_SSID,
    .sta_password = ESPAPERPLAY_SYSTEM_DEFAULT_STA_PASS,
    .ap_ssid = ESPAPERPLAY_SYSTEM_DEFAULT_AP_SSID,
    .ap_password = ESPAPERPLAY_SYSTEM_DEFAULT_AP_PASS,
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
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/** 从 NVS 加载配置；缺失 / 非法的字段回填默认值。 */
static esp_err_t system_load(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPAPERPLAY_SYSTEM_NVS_NAMESPACE, NVS_READONLY, &handle);
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

esp_err_t espaperplay_system_reset_defaults(void) {
    s_config.wifi_mode = ESPAPERPLAY_SYSTEM_DEFAULT_WIFI_MODE;
    strlcpy(s_config.sta_ssid, ESPAPERPLAY_SYSTEM_DEFAULT_STA_SSID, sizeof(s_config.sta_ssid));
    strlcpy(s_config.sta_password, ESPAPERPLAY_SYSTEM_DEFAULT_STA_PASS,
            sizeof(s_config.sta_password));
    strlcpy(s_config.ap_ssid, ESPAPERPLAY_SYSTEM_DEFAULT_AP_SSID, sizeof(s_config.ap_ssid));
    strlcpy(s_config.ap_password, ESPAPERPLAY_SYSTEM_DEFAULT_AP_PASS,
            sizeof(s_config.ap_password));
    return system_save_all();
}
