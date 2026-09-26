/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espaperplay_board.h"
#include "espaperplay_config.h"

static const char *TAG = "ESPaperPlay_BOARD";

/** 外设电源轨（3V3_PER）当前状态。 */
static bool s_rail_on = false;

void espaperplay_board_peripheral_rail_set(bool on) {
    if (on == s_rail_on) {
        return;
    }
    /* POWER_EN 低电平开轨（PMOS 栅极经 470R 拉低导通）。 */
    gpio_set_level(ESPAPERPLAY_PIN_POWER_EN,
                   on ? ESPAPERPLAY_POWER_EN_ACTIVE_LEVEL
                      : !ESPAPERPLAY_POWER_EN_ACTIVE_LEVEL);
    s_rail_on = on;
    if (on) {
        /* 轨稳定等待：EPD / GT911 / SD 均挂本轨，上电到首个外设命令
         * 之间留出裕量（GT911 要求 ≥5ms，SD 卡座去耦充电）。 */
        vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_PERIPHERAL_RAIL_STABILIZE_MS));
    }
}

bool espaperplay_board_peripheral_rail_is_on(void) { return s_rail_on; }

esp_err_t espaperplay_board_init(void) {
    ESP_LOGI(TAG, "%s v%s starting", ESPAPERPLAY_PROJECT_NAME, ESPAPERPLAY_VERSION);

    /* 外设电源轨最先接通：复位后 POWER_EN 悬空、R20 上拉关断，EPD /
     * 触摸 / SD 均无电；任何外设初始化（含下方 SPI 配置产生的引脚
     * 活动）之前必须先恢复供电。深睡关机后由 power 服务负责断轨。 */
    gpio_config_t power_en_cfg = {
        .pin_bit_mask = BIT64(ESPAPERPLAY_PIN_POWER_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&power_en_cfg), TAG, "POWER_EN (GPIO%d) config failed",
                        ESPAPERPLAY_PIN_POWER_EN);
    espaperplay_board_peripheral_rail_set(true);
    ESP_LOGI(TAG, "peripheral rail 3V3_PER on (POWER_EN=GPIO%d)", ESPAPERPLAY_PIN_POWER_EN);

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
