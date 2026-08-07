/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_input.h"
#include "esp_log.h"

static const char *TAG = "ESPaperPlay_INPUT";

esp_err_t espaperplay_input_init(void) {
    ESP_LOGI(TAG, "Input subsystem init (skeleton)");
    /* TODO(阶段2): 创建事件队列并注册触摸 / 按键事件源。 */
    return ESP_OK;
}

esp_err_t espaperplay_input_get_event(espaperplay_input_event_t *event, uint32_t timeout_ms) {
    (void)event;
    (void)timeout_ms;

    ESP_LOGW(TAG, "input_get_event not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}
