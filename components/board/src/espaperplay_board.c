/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "driver/spi_common.h"
#include "driver/spi_master.h"

#include "esp_check.h"
#include "esp_log.h"

#include "espaperplay_board.h"
#include "espaperplay_config.h"

static const char *TAG = "ESPaperPlay_BOARD";

esp_err_t espaperplay_board_init(void) {
    ESP_LOGI(TAG, "%s v%s starting", ESPAPERPLAY_PROJECT_NAME, ESPAPERPLAY_VERSION);

    /* 初始化 SPI 主机（EPD 独占 SPI2；SD 卡走独立 SDMMC/SDIO 主机，
     * 引脚见 espaperplay_config.h 的 SD 节）。EPD 只写不读，MISO 未接。 */
    const spi_bus_config_t buscfg = {
        .sclk_io_num = ESPAPERPLAY_PIN_EPD_SCLK,
        .mosi_io_num = ESPAPERPLAY_PIN_EPD_MOSI,
        .miso_io_num = ESPAPERPLAY_PIN_EPD_MISO, /* -1：EPD 无读取需求 */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ESPAPERPLAY_EPD_SPI_MAX_TRANSFER,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize((spi_host_device_t)ESPAPERPLAY_SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO),
        TAG, "SPI host SPI%d init failed", (int)ESPAPERPLAY_SPI_HOST_ID);
    ESP_LOGI(TAG, "SPI host SPI%d ready (SCLK=%d MOSI=%d MISO=%d)",
             (int)ESPAPERPLAY_SPI_HOST_ID, ESPAPERPLAY_PIN_EPD_SCLK, ESPAPERPLAY_PIN_EPD_MOSI,
             ESPAPERPLAY_PIN_EPD_MISO);

    /* 注：I2C 主机（GT911）由 touch 组件在 espaperplay_touch_init() 内自建，
     *     后续若改为板级统一管理，需同步迁移。 */

    ESP_LOGI(TAG, "Board bring-up done");
    return ESP_OK;
}
