/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "espaperplay_nvs.h"

static const char *TAG = "ESPaperPlay_NVS";

/** 应用层命名空间清单（顺序无关；须与 espaperplay_nvs.h 中宏一致）。 */
static const char *const s_app_namespaces[] = {
    ESPAPERPLAY_NVS_NS_SYSTEM,
    ESPAPERPLAY_NVS_NS_AUTH,
    ESPAPERPLAY_NVS_NS_CLOCK,
    ESPAPERPLAY_NVS_NS_TLS,
};

esp_err_t espaperplay_nvs_factory_reset(void) {
    esp_err_t last_err = ESP_OK;
    for (size_t i = 0; i < sizeof(s_app_namespaces) / sizeof(s_app_namespaces[0]); i++) {
        nvs_handle_t handle;
        esp_err_t err = nvs_open(s_app_namespaces[i], NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "open ns \"%s\" failed: %s", s_app_namespaces[i], esp_err_to_name(err));
            last_err = err;
            continue;
        }
        err = nvs_erase_all(handle);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK; /* 本就无该命名空间，视为已清空 */
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "erase ns \"%s\" failed: %s", s_app_namespaces[i], esp_err_to_name(err));
            last_err = err;
        } else {
            ESP_LOGI(TAG, "erased nvs namespace \"%s\"", s_app_namespaces[i]);
        }
    }
    return last_err;
}
