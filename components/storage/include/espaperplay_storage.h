/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "espaperplay_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_storage.h
 * @brief 存储抽象层。
 *
 * 在板载 SD 卡之上提供面向文件系统的接口。SD 卡通过 ESP-IDF VFS 暴露在
 * ESPAPERPLAY_STORAGE_MOUNT_POINT 挂载点，使内容（EPUB / TXT / 图片 /
 * 配置文件）可通过标准 C 文件 API 打开。SD SPI + FAT 文件系统实现稍后添加。
 */

/**
 * @brief 挂载 SD 卡并注册到 VFS。
 *
 * @note 当前仅为骨架：SDMMC/SPI + FATFS 挂载尚未实现。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_storage_mount(void);

/**
 * @brief 卸载 SD 卡并释放相关资源。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_storage_unmount(void);

/**
 * @brief 检查存储当前是否已挂载。
 *
 * @return 已挂载返回 true，否则返回 false。
 */
bool espaperplay_storage_is_mounted(void);

#ifdef __cplusplus
}
#endif
