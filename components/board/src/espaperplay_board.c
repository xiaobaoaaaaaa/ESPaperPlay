/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_board.h"
#include "esp_log.h"
#include "espaperplay_config.h"

static const char *TAG = "ESPaperPlay_BOARD";

esp_err_t espaperplay_board_init(void) {
    ESP_LOGI(TAG, "%s v%s starting", ESPAPERPLAY_PROJECT_NAME, ESPAPERPLAY_VERSION);

    /* TODO(阶段2): 初始化 GPIO 引脚复用。 */
    /* TODO(阶段2): 初始化 SPI 主机（EPD 与 SD 卡共用 SPI2）。 */
    /* TODO(阶段2): 初始化 I2C 主机（GT911 触摸控制器）。 */

    ESP_LOGI(TAG, "Board bring-up done (skeleton)");
    return ESP_OK;
}
