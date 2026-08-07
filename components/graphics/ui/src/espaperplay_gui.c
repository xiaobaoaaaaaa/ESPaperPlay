/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_gui.h"
#include "esp_log.h"

static const char *TAG = "ESPaperPlay_GUI";

esp_err_t espaperplay_gui_init(void) {
    ESP_LOGI(TAG, "GUI init (skeleton, LVGL pending)");
    /* TODO(阶段2): 分配帧缓冲（PSRAM）+ LVGL 移植。 */
    return ESP_OK;
}

esp_err_t espaperplay_gui_start(void) {
    ESP_LOGW(TAG, "gui_start not implemented yet (LVGL task pending)");
    return ESP_ERR_NOT_SUPPORTED;
}
