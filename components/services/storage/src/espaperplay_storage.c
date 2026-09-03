/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "esp_vfs_fat.h"  /* esp_vfs_fat_register / esp_vfs_fat_unregister_path */
#include "diskio_impl.h"   /* ff_diskio_get_drive / ff_diskio_unregister */
#include "diskio_sdmmc.h"  /* ff_diskio_register_sdmmc */
#include "ff.h"            /* f_mount / FATFS / FRESULT / FF_DRV_NOT_USED */

#include "esp_check.h"
#include "esp_log.h"

#include "espaperplay_config.h"
#include "espaperplay_sd.h"
#include "espaperplay_storage.h"

static const char *TAG = "ESPaperPlay_STORAGE";

static bool s_mounted = false;      /*!< FAT 卷已挂载 + VFS 已注册 */
static BYTE s_pdrv = FF_DRV_NOT_USED; /*!< 分配的 FAT 卷号（0..FF_VOLUMES-1） */

/* ====================================================================
 * 存储自检（仅 ESPAPERPLAY_STORAGE_ENABLE_SELFTEST=1 时启用）
 *
 * 挂载成功后写入临时文件、读回校验并删除：覆盖 SDIO 传输 → FATFS →
 * VFS 全链路。非破坏性（临时文件随后删除）。
 * ==================================================================== */
#if ESPAPERPLAY_STORAGE_ENABLE_SELFTEST
static esp_err_t storage_selftest(void) {
    static const char test_path[] =
        ESPAPERPLAY_STORAGE_MOUNT_POINT "/espaperplay_selftest.txt";
    static const char test_content[] = "ESPaperPlay SDIO storage self-test OK\n";

    FILE *f = fopen(test_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "self-test: open for write failed");
        return ESP_FAIL;
    }
    size_t written = fwrite(test_content, 1, sizeof(test_content) - 1, f);
    fclose(f);
    if (written != sizeof(test_content) - 1) {
        ESP_LOGE(TAG, "self-test: short write (%u/%u)", (unsigned)written,
                 (unsigned)(sizeof(test_content) - 1));
        remove(test_path);
        return ESP_FAIL;
    }

    char buf[sizeof(test_content)] = {0};
    f = fopen(test_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "self-test: open for read failed");
        remove(test_path);
        return ESP_FAIL;
    }
    size_t read_bytes = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(test_path);

    if (read_bytes != sizeof(test_content) - 1 ||
        memcmp(buf, test_content, sizeof(test_content) - 1) != 0) {
        ESP_LOGE(TAG, "self-test: content mismatch");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "storage self-test PASS (write/read/delete %s)", test_path);
    return ESP_OK;
}
#endif /* ESPAPERPLAY_STORAGE_ENABLE_SELFTEST */

esp_err_t espaperplay_storage_mount(void) {
    if (s_mounted) {
        ESP_LOGW(TAG, "Storage already mounted");
        return ESP_OK;
    }

    /* 1. 底层 SDIO 驱动：电源轨 + SDMMC 主机/槽位 + 卡片探测 */
    esp_err_t ret = espaperplay_sd_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card init failed: %s (is a card inserted?)", esp_err_to_name(ret));
        return ret;
    }
    sdmmc_card_t *card = espaperplay_sd_get_card();
    if (card == NULL) {
        ESP_LOGE(TAG, "SD card handle unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    /* 2. 分配 FAT 卷号（0..FF_VOLUMES-1） */
    BYTE pdrv = FF_DRV_NOT_USED;
    ret = ff_diskio_get_drive(&pdrv);
    if (ret != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        ESP_LOGE(TAG, "no free FAT drive slot available");
        return ESP_ERR_NO_MEM;
    }
    char drv[3] = {(char)('0' + pdrv), ':', 0};

    /* 3. 把 SD 卡注册为 FATFS 底层磁盘（块设备接口） */
    ff_diskio_register_sdmmc(pdrv, card);

    /* 4. 把 FATFS 卷挂到 VFS 挂载点 */
    FATFS *fs = NULL;
    const esp_vfs_fat_conf_t conf = {
        .base_path = ESPAPERPLAY_STORAGE_MOUNT_POINT,
        .fat_drive = drv,
        .max_files = ESPAPERPLAY_STORAGE_MAX_OPEN_FILES,
    };
    ret = esp_vfs_fat_register(&conf, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_register failed: %s", esp_err_to_name(ret));
        goto fail_after_register_attempt;
    }
    if (fs == NULL) {
        ESP_LOGE(TAG, "esp_vfs_fat_register returned NULL fs");
        ret = ESP_ERR_INVALID_STATE;
        goto fail_after_register_attempt;
    }

    /* 5. 挂载文件系统（不自动格式化：避免误格式化已有数据的卡片） */
    FRESULT fres = f_mount(fs, drv, 1);
    if (fres != FR_OK) {
        ESP_LOGE(TAG, "f_mount failed (FRESULT=%d): card has no FAT filesystem?\n"
                 "Please format the card as FAT32 (e.g. mkfs.vfat / SD Card Formatter)",
                 (int)fres);
        ret = ESP_FAIL;
        goto fail_after_mount;
    }

    s_pdrv = pdrv;
    s_mounted = true;

    ESP_LOGI(TAG, "SD card mounted at %s (FAT drive %s, %u open files max)",
             ESPAPERPLAY_STORAGE_MOUNT_POINT, drv, (unsigned)conf.max_files);

#if ESPAPERPLAY_STORAGE_ENABLE_SELFTEST
    esp_err_t st_ret = storage_selftest();
    if (st_ret != ESP_OK) {
        ESP_LOGW(TAG, "storage self-test FAILED: %s", esp_err_to_name(st_ret));
    }
#endif

    return ESP_OK;

fail_after_mount:
    f_mount(NULL, drv, 0);
    esp_vfs_fat_unregister_path(ESPAPERPLAY_STORAGE_MOUNT_POINT);
fail_after_register_attempt:
    ff_diskio_unregister(pdrv);
    s_pdrv = FF_DRV_NOT_USED;
    espaperplay_sd_deinit();
    return ret;
}

esp_err_t espaperplay_storage_unmount(void) {
    if (!s_mounted) {
        ESP_LOGW(TAG, "Storage not mounted, nothing to unmount");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    if (s_pdrv != FF_DRV_NOT_USED) {
        char drv[3] = {(char)('0' + s_pdrv), ':', 0};
        f_mount(NULL, drv, 0);
        ff_diskio_unregister(s_pdrv);
        s_pdrv = FF_DRV_NOT_USED;
    }
    esp_err_t vfs_ret = esp_vfs_fat_unregister_path(ESPAPERPLAY_STORAGE_MOUNT_POINT);
    if (vfs_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_unregister_path failed: %s", esp_err_to_name(vfs_ret));
        ret = vfs_ret;
    }
    esp_err_t sd_ret = espaperplay_sd_deinit();
    if (sd_ret != ESP_OK) {
        ret = sd_ret;
    }

    s_mounted = false;
    ESP_LOGI(TAG, "SD card unmounted");
    return ret;
}

bool espaperplay_storage_is_mounted(void) { return s_mounted; }