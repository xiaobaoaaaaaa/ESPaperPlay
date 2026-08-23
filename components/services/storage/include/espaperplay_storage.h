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
 * @brief 启用「下电-重挂载」自检（验收用，默认关闭）。
 *
 * 使能后，espaperplay_storage_mount() 在挂载成功后执行一轮完整下电验证：
 * 写入标记文件 → 完整下电（espaperplay_storage_unmount()：CTRL_SYNC 刷盘
 * → 卸载 FAT/VFS → SD 驱动 CMD0 软下电 + 停主机）→ 重新挂载 → 读回校验
 * 内容一致 → 删除标记文件。用于确认 SD 卡下电不丢数据、且软下电后的卡片
 * 可被重新探测挂载。验收完成后置 0 关闭（默认）。
 *
 * @note 自检期间 SD 卡会短暂经历一次卸载-重挂载；此时字体组件的 SD 完整
 *       字库盘符 'B:' 会瞬时不可用（访问失败自动回退 Flash 字体），自检
 *       结束后恢复正常。仅在验收时开启即可。
 */
#ifndef ESPAPERPLAY_STORAGE_ENABLE_PWR_CYCLE_SELFTEST
#define ESPAPERPLAY_STORAGE_ENABLE_PWR_CYCLE_SELFTEST 0
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
 * @brief 卸载 SD 卡并执行完整下电序列（模拟断电）。
 *
 * 下电顺序：
 *  1. disk_ioctl(CTRL_SYNC)：全局刷盘，确保 FATFS 无未落盘数据；
 *  2. f_mount(NULL) + 注销底层磁盘驱动；
 *  3. 注销 VFS 挂载点；
 *  4. SD 驱动下电（espaperplay_sd_deinit()）：CMD0 让卡片回到 idle 态
 *     （协议级"模拟断电"，硬件暂无负载开关时的等效断电）→ 停止 SDMMC
 *     主机 → 电源轨下电（接入负载开关后自动变为真实断电）。
 *
 * 调用前应确保所有文件句柄已关闭（f_close 内部会同步数据）；本函数再以
 * CTRL_SYNC 兜底。适用于重启前、深度睡眠前等需要安全下电的场景。
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