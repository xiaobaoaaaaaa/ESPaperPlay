/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "esp_log.h"
#include "espaperplay_touch.h"

static const char *TAG = "ESPaperPlay_TOUCH";

esp_err_t espaperplay_touch_init(void) {
    ESP_LOGI(TAG, "GT911 touch init (skeleton, driver pending)");
    /* TODO(阶段2): 配置 I2C 总线，复位 GT911 并回读能力寄存器。 */
    return ESP_OK;
}

esp_err_t espaperplay_touch_read(espaperplay_touch_point_t *points, uint8_t max_points,
                                 uint8_t *count) {
    (void)points;
    (void)max_points;

    ESP_LOGW(TAG, "touch_read not implemented yet (driver pending)");
    if (count != NULL) {
        *count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
