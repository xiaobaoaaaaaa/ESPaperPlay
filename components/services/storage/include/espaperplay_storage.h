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
 * @brief 存储抽象层（MicroSD + FAT 文件系统）。
 *
 * 在 MicroSD 卡驱动（components/drivers/sd，SDIO/SDMMC 接口）之上提供
 * 面向文件系统的接口：挂载 FAT 卷并注册到 ESP-IDF VFS，挂载点见
 * ESPAPERPLAY_STORAGE_MOUNT_POINT（默认 /sdcard）。挂载后内容（EPUB /
 * TXT / 图片 / 配置文件）可通过标准 C 文件 API（fopen / fread / fwrite...）
 * 直接访问。
 *
 * 依赖关系：storage → sd 驱动（SDIO 主机/卡片）→ board（引脚/时钟）。
 * storage 只负责文件系统与 VFS 层，不直接触碰硬件寄存器。
 */
#define ESPAPERPLAY_STORAGE_MAX_OPEN_FILES 8

/**
 * @brief 启用存储自检（验收用，默认关闭）。
 *
 * 使能后，espaperplay_storage_mount() 在挂载成功后于挂载点根目录写入
 * 临时文件 espaperplay_selftest.txt、读回校验内容并删除，验证
 * SDIO 传输 → FATFS → VFS 全链路。验收完成后置 0 关闭（默认）；需要
 * 复验时改回 1 重新编译即可。
 */
#ifndef ESPAPERPLAY_STORAGE_ENABLE_SELFTEST
#define ESPAPERPLAY_STORAGE_ENABLE_SELFTEST 0
#endif

/**
 * @brief 挂载 SD 卡并注册到 VFS（FAT 文件系统）。
 *
 * 依次完成：SD 驱动初始化（电源轨 + SDMMC 主机/槽位 + SDIO 卡片探测）
 * → 分配 FAT 卷号并注册底层磁盘驱动 → 注册 VFS 挂载点 → f_mount 挂载。
 *
 * 不做自动格式化：卡片无文件系统（或文件系统损坏）时返回错误，避免误格式化
 * 已有数据的卡片；用户可自行用 FAT32 格式化的卡或调用底层工具修复。
 *
 * @note 无卡片或卡片初始化失败时返回错误（ESP_ERR_TIMEOUT 等），调用方
 *       应容忍「无存储」启动（设备其余功能不受影响）。
 *
 * @return 成功返回 ESP_OK，否则返回对应错误码。
 */
esp_err_t espaperplay_storage_mount(void);

/**
 * @brief 卸载 SD 卡并释放相关资源。
 *
 * 先卸载 FATFS 卷并注销 VFS 挂载点，再反初始化 SD 驱动（主机/槽位 +
 * 电源轨下电）。
 *
 * @return 成功返回 ESP_OK，否则返回对应错误码。
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