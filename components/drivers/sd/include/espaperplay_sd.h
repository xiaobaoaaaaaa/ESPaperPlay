/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sd_protocol_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_sd.h
 * @brief MicroSD 卡驱动（SDIO / SDMMC 接口）。
 *
 * 驱动基于 ESP32-S3 片上 SDMMC 主机（SDIO 协议栈，非 SPI），提供卡片的
 * 上电、主机/槽位初始化、卡片探测与底层扇区读写。引脚与时钟等板级参数
 * 见 espaperplay_config.h 的 SD 节；上层文件系统（FAT + VFS）由 storage
 * 服务在 driver 之上构建（espaperplay_storage_mount()）。
 *
 * 时序：sdmmc_host_init() → sdmmc_host_init_slot()
 * → sdmmc_card_init()（SDIO 初始化序列，含 CMD0/CMD8/ACMD41/CMD2/CMD3/
 * CMD9/CMD7）。全部成功后方可进行扇区读写。
 */

/**
 * @brief 启用驱动级自检（验收用，默认关闭）。
 *
 * 使能后，espaperplay_sd_init() 在卡片初始化成功后创建一个后台任务，
 * 打印卡片信息并只读校验扇区 0（MBR）与最后一扇区，验证 SDIO 传输链路；
 * 自检不写入任何数据，非破坏性。验收完成后置 0 关闭（默认）；需要复验时
 * 改回 1 重新编译即可。
 */
#ifndef ESPAPERPLAY_SD_ENABLE_SELFTEST
#define ESPAPERPLAY_SD_ENABLE_SELFTEST 0
#endif

/**
 * @brief 初始化 MicroSD 卡（SDIO/SDMMC）。
 *
 * 依次完成：SDMMC 主机初始化 → 槽位配置（引脚/总线宽度/
 * 内部上拉）→ SDIO 卡片初始化（探测并识别卡片）。成功后打印卡片信息
 * （卡型 / 容量 / 支持的速率等）。
 *
 * @return 成功返回 ESP_OK；卡片不存在或初始化失败返回对应错误码
 *         （常见 ESP_ERR_TIMEOUT / ESP_ERR_NOT_FOUND / ESP_FAIL）。
 */
esp_err_t espaperplay_sd_init(void);

/**
 * @brief 反初始化 MicroSD 卡（SDIO/SDMMC）。
 *
 * 释放槽位与主机。调用前必须确保文件系统已卸载
 * （espaperplay_storage_unmount() 负责在卸载 VFS/FAT 后调用本函数）。
 *
 * @return 成功返回 ESP_OK，否则返回对应错误码。
 */
esp_err_t espaperplay_sd_deinit(void);

/**
 * @brief 检查 SD 卡当前是否已初始化（探测成功）。
 *
 * @return 已初始化返回 true，否则返回 false。
 */
bool espaperplay_sd_is_detected(void);

/**
 * @brief 获取 SD 卡信息结构体指针。
 *
 * 供 storage 服务注册 FAT 磁盘驱动（ff_diskio_register_sdmmc）等上层
 * 使用。调用前请先用 espaperplay_sd_is_detected() 确认卡片已就绪。
 *
 * @return 已初始化时返回内部 sdmmc_card_t 指针，否则返回 NULL。
 */
sdmmc_card_t *espaperplay_sd_get_card(void);

/**
 * @brief 读取若干扇区（底层块设备接口，不经文件系统）。
 *
 * 用于自检 / 诊断 / 未来的分区管理等场景；文件读写请使用 VFS
 * 挂载点（ESPAPERPLAY_STORAGE_MOUNT_POINT）上的标准 C 文件 API。
 *
 * @param start_sector 起始扇区号（0 起）。
 * @param sector_count 要读取的扇区数。
 * @param[out] dst     接收数据的缓冲区（大小 >= sector_count * 512）。
 *
 * @return 成功返回 ESP_OK；驱动未就绪返回 ESP_ERR_INVALID_STATE，
 *         其余错误返回对应错误码。
 */
esp_err_t espaperplay_sd_read_sectors(size_t start_sector, size_t sector_count, void *dst);

/**
 * @brief 写入若干扇区（底层块设备接口，不经文件系统）。
 *
 * 破坏性操作：会覆盖卡片上的原有数据，务必谨慎使用。
 *
 * @param start_sector 起始扇区号（0 起）。
 * @param sector_count 要写入的扇区数。
 * @param[in] src      待写入数据的缓冲区（大小 >= sector_count * 512）。
 *
 * @return 成功返回 ESP_OK；驱动未就绪返回 ESP_ERR_INVALID_STATE，
 *         其余错误返回对应错误码。
 */
esp_err_t espaperplay_sd_write_sectors(size_t start_sector, size_t sector_count, const void *src);

#ifdef __cplusplus
}
#endif