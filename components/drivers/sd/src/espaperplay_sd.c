/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h" /* SDMMC 主机（SDIO）驱动，legacy 公共 API */

#include "sdmmc_cmd.h" /* 卡片级 API：sdmmc_card_init / 扇区读写 / 信息打印 */

#include "esp_check.h"
#include "esp_log.h"

#include "espaperplay_config.h"
#include "espaperplay_sd.h"

static const char *TAG = "ESPaperPlay_SD";

/* ====================================================================
 * 内部状态
 * ==================================================================== */

static sdmmc_host_t s_host;        /*!< SDMMC 主机配置（SDMMC_HOST_DEFAULT + 板级参数） */
static sdmmc_card_t s_card;        /*!< 卡片信息（sdmmc_card_init 填充，内部已清零+拷贝 host） */
static bool s_host_ready = false;  /*!< 主机已初始化（sdmmc_host_init） */
static bool s_slot_ready = false;  /*!< 槽位已初始化（sdmmc_host_init_slot） */
static bool s_card_ready = false;  /*!< 卡片已探测成功（sdmmc_card_init） */

/* ====================================================================
 * 驱动级自检（仅 ESPAPERPLAY_SD_ENABLE_SELFTEST=1 时启用）
 *
 * 非破坏性：只读校验扇区 0（MBR）与最后一扇区，验证 SDIO 传输链路。
 * 不写入任何数据，可安全地在有内容的卡片上运行。
 * ==================================================================== */
#if ESPAPERPLAY_SD_ENABLE_SELFTEST
static void sd_selftest_task(void *arg) {
    (void)arg;

    if (!s_card_ready) {
        ESP_LOGE(TAG, "self-test: card not ready");
        vTaskDelete(NULL);
        return;
    }

    const size_t sector_size = (size_t)s_card.csd.sector_size;
    const size_t sector_count = (size_t)s_card.csd.capacity;
    ESP_LOGI(TAG, "self-test: card has %u sectors x %u bytes",
             (unsigned)sector_count, (unsigned)sector_size);

    uint8_t *buf = malloc(sector_size);
    if (buf == NULL) {
        ESP_LOGE(TAG, "self-test: buffer alloc failed (%u bytes)", (unsigned)sector_size);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = sdmmc_read_sectors(&s_card, buf, 0, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "self-test: read sector 0 failed: %s", esp_err_to_name(err));
        free(buf);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "self-test: sector 0 head: %02X %02X %02X %02X %02X %02X %02X %02X",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);

    if (sector_count > 1) {
        err = sdmmc_read_sectors(&s_card, buf, sector_count - 1, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "self-test: read last sector failed: %s", esp_err_to_name(err));
            free(buf);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "self-test: last sector head: %02X %02X %02X %02X %02X %02X %02X %02X",
                 buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
    }

    free(buf);
    ESP_LOGI(TAG, "SD self-test PASS");
    vTaskDelete(NULL);
}
#endif /* ESPAPERPLAY_SD_ENABLE_SELFTEST */

/* ====================================================================
 * 公共 API
 * ==================================================================== */

esp_err_t espaperplay_sd_init(void) {
    if (s_card_ready) {
        ESP_LOGW(TAG, "SD card already initialized");
        return ESP_OK;
    }
    if (s_host_ready || s_slot_ready) {
        ESP_LOGW(TAG, "SD host/slot already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing MicroSD card: SDIO/SDMMC slot %d, %d-bit, %lu kHz",
             ESPAPERPLAY_SDMMC_HOST_SLOT, ESPAPERPLAY_SD_BUS_WIDTH,
             (unsigned long)(ESPAPERPLAY_SD_CLK_HZ / 1000));

    esp_err_t ret;
    bool host_inited = false;

    /* 1. SDMMC 主机初始化 */
    s_host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    s_host.slot = ESPAPERPLAY_SDMMC_HOST_SLOT;
    s_host.max_freq_khz = ESPAPERPLAY_SD_CLK_HZ / 1000;
    ret = sdmmc_host_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_host_init failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_host_ready = true;
    host_inited = true;

    /* 2. 槽位配置：引脚 / 总线宽度 / 内部上拉（SDIO 协议要求 CMD+D0-D3 上拉） */
    if (ESPAPERPLAY_SD_BUS_WIDTH != 1 && ESPAPERPLAY_SD_BUS_WIDTH != 4) {
        ESP_LOGE(TAG, "invalid bus width %d (only 1 or 4 supported)", ESPAPERPLAY_SD_BUS_WIDTH);
        ret = ESP_ERR_INVALID_ARG;
        goto fail;
    }
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = ESPAPERPLAY_PIN_SD_CLK;
    slot.cmd = ESPAPERPLAY_PIN_SD_CMD;
    slot.d0 = ESPAPERPLAY_PIN_SD_D0;
    slot.d1 = (ESPAPERPLAY_SD_BUS_WIDTH >= 4) ? ESPAPERPLAY_PIN_SD_D1 : GPIO_NUM_NC;
    slot.d2 = (ESPAPERPLAY_SD_BUS_WIDTH >= 4) ? ESPAPERPLAY_PIN_SD_D2 : GPIO_NUM_NC;
    slot.d3 = (ESPAPERPLAY_SD_BUS_WIDTH >= 4) ? ESPAPERPLAY_PIN_SD_D3 : GPIO_NUM_NC;
    slot.width = ESPAPERPLAY_SD_BUS_WIDTH;
    slot.cd = SDMMC_SLOT_NO_CD; /* 无卡检测线 */
    slot.wp = SDMMC_SLOT_NO_WP; /* 无写保护线 */
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    ret = sdmmc_host_init_slot(ESPAPERPLAY_SDMMC_HOST_SLOT, &slot);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_host_init_slot failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_slot_ready = true;

    /* 3. SDIO 卡片初始化（CMD0/CMD8/ACMD41/CMD2/CMD3/CMD9/CMD7 探测序列） */
    ret = sdmmc_card_init(&s_host, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "sdmmc_card_init failed: %s (card absent or unresponsive?)",
                 esp_err_to_name(ret));
        goto fail;
    }
    s_card_ready = true;

    sdmmc_card_print_info(stdout, &s_card);

#if ESPAPERPLAY_SD_ENABLE_SELFTEST
    if (xTaskCreate(sd_selftest_task, "sd_selftest", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "self-test task create failed");
    }
#endif

    ESP_LOGI(TAG, "MicroSD card ready (SDIO)");
    return ESP_OK;

fail:
    /* 收尾统一走 sdmmc_host_deinit()：它会移除仍注册的槽位并删除控制器。
     *
     * 不能先 sdmmc_host_deinit_slot() 再 sdmmc_host_deinit()——
     * IDF v6.0.2 legacy shim 中 deinit_slot 在最后一个槽位被移除后就会
     * 释放控制器（registered_slot_nums==0），但 legacy 静态 s_ctlr 不会
     * 置空，随后的 deinit() 会再次删除同一控制器 → use-after-free →
     * 执行流跳入已释放内存导致 InstructionFetchError 崩溃（无卡时必现）。
     * 单槽位场景直接用 deinit() 一次完成即可（与 IDF 示例配对方式一致）。 */
    if (host_inited) {
        sdmmc_host_deinit();
    }
    s_host_ready = false;
    s_slot_ready = false;
    s_card_ready = false;
    return ret;
}

esp_err_t espaperplay_sd_deinit(void) {
    if (!s_host_ready && !s_slot_ready && !s_card_ready) {
        ESP_LOGW(TAG, "SD not initialized, nothing to deinit");
        return ESP_OK;
    }

    /* 同样只用 sdmmc_host_deinit() 收尾（切勿 deinit_slot+deinit 连用，
     * 见 espaperplay_sd_init 失败路径的注释：会双重删除控制器）。 */
    if (s_host_ready || s_slot_ready || s_card_ready) {
        sdmmc_host_deinit();
    }
    s_host_ready = false;
    s_slot_ready = false;
    s_card_ready = false;

    ESP_LOGI(TAG, "MicroSD deinitialized (SDIO)");
    return ESP_OK;
}

bool espaperplay_sd_is_detected(void) { return s_card_ready; }

sdmmc_card_t *espaperplay_sd_get_card(void) {
    return s_card_ready ? &s_card : NULL;
}

esp_err_t espaperplay_sd_read_sectors(size_t start_sector, size_t sector_count, void *dst) {
    if (!s_card_ready) {
        ESP_LOGE(TAG, "card not ready, cannot read");
        return ESP_ERR_INVALID_STATE;
    }
    if (dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return sdmmc_read_sectors(&s_card, dst, start_sector, sector_count);
}

esp_err_t espaperplay_sd_write_sectors(size_t start_sector, size_t sector_count,
                                       const void *src) {
    if (!s_card_ready) {
        ESP_LOGE(TAG, "card not ready, cannot write");
        return ESP_ERR_INVALID_STATE;
    }
    if (src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return sdmmc_write_sectors(&s_card, src, start_sector, sector_count);
}