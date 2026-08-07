/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_reader.h"
#include "esp_log.h"

static const char *TAG = "ESPaperPlay_READER";

esp_err_t espaperplay_reader_init(void) {
    ESP_LOGI(TAG, "Reader framework init (skeleton)");
    /* TODO(阶段2): 将 storage + input + gui 串联起来。 */
    return ESP_OK;
}

esp_err_t espaperplay_reader_open(const char *path) {
    (void)path;

    ESP_LOGW(TAG, "reader_open not implemented yet (document support pending)");
    return ESP_ERR_NOT_SUPPORTED;
}
