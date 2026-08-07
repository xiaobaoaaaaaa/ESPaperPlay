/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_storage.h"
#include "esp_log.h"

static const char *TAG = "ESPaperPlay_STORAGE";

static bool s_mounted = false;

esp_err_t espaperplay_storage_mount(void) {
    if (s_mounted) {
        ESP_LOGW(TAG, "Storage already mounted");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SD card mount (skeleton, FATFS pending), mount point: %s",
             ESPAPERPLAY_STORAGE_MOUNT_POINT);
    /* TODO(阶段2): SPI SD 初始化 + FATFS 挂载 + VFS 注册。 */
    s_mounted = true;
    return ESP_OK;
}

esp_err_t espaperplay_storage_unmount(void) {
    ESP_LOGW(TAG, "storage_unmount not implemented yet");
    s_mounted = false;
    return ESP_ERR_NOT_SUPPORTED;
}

bool espaperplay_storage_is_mounted(void) { return s_mounted; }
