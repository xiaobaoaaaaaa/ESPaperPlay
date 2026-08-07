/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_epd.h"
#include "esp_log.h"

static const char *TAG = "ESPaperPlay_EPD";

esp_err_t espaperplay_epd_init(void) {
    ESP_LOGI(TAG, "EPD init: %ux%u (skeleton, UC8179 driver pending)", ESPAPERPLAY_DISPLAY_WIDTH,
             ESPAPERPLAY_DISPLAY_HEIGHT);
    /* TODO(阶段2): 电源轨上电、硬件复位、UC8179 初始化时序。 */
    return ESP_OK;
}

esp_err_t espaperplay_epd_refresh(const void *image_buf, uint16_t x, uint16_t y, uint16_t width,
                                  uint16_t height, espaperplay_epd_mode_t mode) {
    (void)image_buf;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)mode;

    ESP_LOGW(TAG, "epd_refresh not implemented yet (driver pending)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espaperplay_epd_sleep(void) {
    ESP_LOGW(TAG, "epd_sleep not implemented yet (driver pending)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espaperplay_epd_power_off(void) {
    ESP_LOGW(TAG, "epd_power_off not implemented yet (driver pending)");
    return ESP_ERR_NOT_SUPPORTED;
}
