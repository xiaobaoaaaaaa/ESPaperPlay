/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_power.h"
#include "esp_log.h"

static const char *TAG = "ESPaperPlay_POWER";

esp_err_t espaperplay_power_init(void) {
    ESP_LOGI(TAG, "Power management init (skeleton)");
    /* TODO(阶段2): 配置电源域 GPIO 电源轨与唤醒 GPIO。 */
    return ESP_OK;
}

esp_err_t espaperplay_power_domain_set(espaperplay_power_domain_t domain, bool enable) {
    (void)domain;
    (void)enable;

    ESP_LOGW(TAG, "power_domain_set not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espaperplay_power_configure_wakeup(const espaperplay_wakeup_config_t *config) {
    (void)config;

    ESP_LOGW(TAG, "power_configure_wakeup not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espaperplay_power_enter_sleep(void) {
    ESP_LOGW(TAG, "power_enter_sleep not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}
