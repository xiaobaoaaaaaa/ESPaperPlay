/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"

#include "espaperplay_config.h"
#include "espaperplay_display.h"

static const char *TAG = "ESPaperPlay_BOARD";

/* 默认 = 板级配置宏；set() 覆盖（须在子系统初始化之前）。 */
static uint16_t s_disp_w = ESPAPERPLAY_DISPLAY_WIDTH;
static uint16_t s_disp_h = ESPAPERPLAY_DISPLAY_HEIGHT;

esp_err_t espaperplay_display_set(uint16_t width, uint16_t height) {
    if (width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (width != s_disp_w || height != s_disp_h) {
        ESP_LOGI(TAG, "display resolution: %ux%u -> %ux%u", s_disp_w, s_disp_h, width, height);
        s_disp_w = width;
        s_disp_h = height;
    }
    return ESP_OK;
}

uint16_t espaperplay_display_width(void) {
    return s_disp_w;
}

uint16_t espaperplay_display_height(void) {
    return s_disp_h;
}
