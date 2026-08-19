/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_config.h
 * @brief ESPaperPlay 全局软硬件配置。
 *
 * 本头文件是板级参数的唯一来源：软件版本、屏幕几何参数、GPIO 引脚映射、
 * 以及默认总线（SPI / I2C）设置。板级参数集中于此，使固件其余部分不硬编码
 * 任何硬件细节。
 *
 * @note 下方引脚分配为 *板级默认值*。请对照实际 ESP32-S3 电子纸板原理图核对，
 *       可通过修改本文件或在后续阶段通过 Kconfig 覆盖。
 */

/* ====================================================================
 * 软件版本
 * ==================================================================== */

#define ESPAPERPLAY_VERSION_MAJOR 0
#define ESPAPERPLAY_VERSION_MINOR 1
#define ESPAPERPLAY_VERSION_PATCH 0

/** 人类可读的版本字符串。 */
#define ESPAPERPLAY_VERSION "0.1.0"

/** 项目 / 产品名称。 */
#define ESPAPERPLAY_PROJECT_NAME "ESPaperPlay"

/* ====================================================================
 * 显示屏（GDEY075T7-T01 / UC8179，7.5 英寸 EPD）
 * ==================================================================== */

/** EPD 有效显示区宽度（像素）。 */
#define ESPAPERPLAY_DISPLAY_WIDTH 800

/** EPD 有效显示区高度（像素）。 */
#define ESPAPERPLAY_DISPLAY_HEIGHT 480

/* ====================================================================
 * EPD SPI 接口（独占 SPI2 主机；SD 卡走独立 SDIO/SDMMC，见下方 SD 节）
 * ==================================================================== */

#define ESPAPERPLAY_PIN_EPD_SCLK 12 /*!< SPI 时钟 */
#define ESPAPERPLAY_PIN_EPD_MOSI 11 /*!< SPI MOSI */
#define ESPAPERPLAY_PIN_EPD_MISO -1 /*!< SPI MISO（EPD 只写不读，未使用） */
#define ESPAPERPLAY_PIN_EPD_CS 10   /*!< EPD 片选 */
#define ESPAPERPLAY_PIN_EPD_DC 9    /*!< EPD 数据 / 命令 */
#define ESPAPERPLAY_PIN_EPD_RST 8   /*!< EPD 硬件复位 */
#define ESPAPERPLAY_PIN_EPD_BUSY 7  /*!< EPD 忙状态输入 */
#define ESPAPERPLAY_PIN_EPD_PWR 6   /*!< EPD 电源轨使能 */

/** EPD 默认 SPI 时钟频率，单位 Hz。 */
#define ESPAPERPLAY_EPD_SPI_CLK_HZ 4000000

/** 单次 SPI 事务的最大字节数（EPD 帧缓冲按流式传输）。 */
#define ESPAPERPLAY_EPD_SPI_MAX_TRANSFER 4096

/* ====================================================================
 * 触摸（GT911，I2C）
 * ==================================================================== */

#define ESPAPERPLAY_PIN_TOUCH_SDA 4 /*!< I2C SDA */
#define ESPAPERPLAY_PIN_TOUCH_SCL 5 /*!< I2C SCL */
#define ESPAPERPLAY_PIN_TOUCH_INT 3 /*!< GT911 触摸中断（输入） */
#define ESPAPERPLAY_PIN_TOUCH_RST 2 /*!< GT911 复位（输出） */
#define ESPAPERPLAY_PIN_TOUCH_PWR 1 /*!< 触摸电源轨使能 */

/**
 * GT911 I2C 设备地址（7 位）。
 *
 * GT911 支持两个从机地址，由复位释放瞬间 INT 引脚的电平锁存决定
 * （《GT911 Datasheet》Rev.10 第 6.1 节）：
 *   - 复位释放时 INT 为低电平 → 0xBA/0xBB（7 位 0x5D）；
 *   - 复位释放时 INT 为高电平 → 0x28/0x29（7 位 0x14）。
 *
 * 厂商 STM32 demo（S-GDEY075T7-FP-GT911Touch20230713）在复位期间把 INT
 * 拉低并访问 0xBA/0xBB，本工程沿用该时序，默认地址为 0x5D。touch 驱动
 * 初始化时若 0x5D 探测失败，会按 INT 高电平复位时序自动回退探测 0x14。
 */
#define ESPAPERPLAY_GT911_I2C_ADDR 0x5D

/** 触摸控制器默认 I2C 时钟频率，单位 Hz。 */
#define ESPAPERPLAY_TOUCH_I2C_CLK_HZ 400000

/* ====================================================================
 * SD 卡（SDIO / SDMMC 接口，专用 SDMMC 主机，不再占用 SPI2）
 *
 * ESP32-S3 集成 SDMMC 主机（SOC_SDMMC_HOST_SUPPORTED）：slot 1 支持
 * 1/4-bit，slot 0 仅 1-bit。所有 SDMMC 信号经 GPIO 矩阵路由，可任意选脚。
 *
 * 引脚分配避开已占用 / 不可用的 GPIO：
 *   - EPD SPI（6-13）、触摸（1-5：INT=3 / RST=2 / PWR=1）、BOOT 键 0；
 *   - **GPIO19/20 是芯片内置 USB D-/D+ 焊盘**，被板载 USB（烧录 / 调试 /
 *     次级 USB-Serial-JTAG 串口）占用，严禁用作 SD 信号或电源控制；
 *   - 43/44 为 UART0 串口。CLK/CMD 沿用 IDF 默认 14/15，D0-D3 收敛到
 *     16-18 + 21（21 原是 SPI 方案假定的片选脚，最可能已连到 SD 卡座）。
 *
 * 4-bit 模式要求 CMD/D0-D3 外接 10kΩ 上拉，驱动同时开启内部上拉
 * （SDMMC_SLOT_FLAG_INTERNAL_PULLUP）作为补充。上电前请务必对照原理图
 * 核对引脚；若板载 SD 供电由 GPIO 控制，请把 ESPAPERPLAY_PIN_SD_PWR 改为
 * 实际引脚并置 ESPAPERPLAY_SD_ENABLE_POWER_PIN=1（默认关闭：避免误操作
 * 未知引脚，例如 GPIO20=USB D+ 拉高会导致设备断连）。
 * ==================================================================== */

#define ESPAPERPLAY_PIN_SD_CLK 14 /*!< SDMMC 时钟线（CLK） */
#define ESPAPERPLAY_PIN_SD_CMD 15 /*!< SDMMC 命令线（CMD） */
#define ESPAPERPLAY_PIN_SD_D0 16  /*!< SDMMC 数据线 D0 */
#define ESPAPERPLAY_PIN_SD_D1 17  /*!< SDMMC 数据线 D1 */
#define ESPAPERPLAY_PIN_SD_D2 18  /*!< SDMMC 数据线 D2 */
#define ESPAPERPLAY_PIN_SD_D3 21  /*!< SDMMC 数据线 D3 */
#define ESPAPERPLAY_PIN_SD_PWR -1 /*!< SD 卡电源轨使能（-1=无；按原理图配置实际引脚） */

/** SDMMC 主机槽位（ESP32-S3：slot 1 支持 4-bit，slot 0 仅 1-bit）。 */
#define ESPAPERPLAY_SDMMC_HOST_SLOT 1

/** SD 卡 SDMMC 总线宽度（1 或 4；4 线最高 40MHz SDR）。 */
#define ESPAPERPLAY_SD_BUS_WIDTH 4

/** SD 卡默认 SDMMC 时钟频率，单位 Hz（SDMMC 默认 20MHz，上限 40MHz）。 */
#define ESPAPERPLAY_SD_CLK_HZ 20000000

/** SD 卡的 VFS 挂载点。 */
#define ESPAPERPLAY_STORAGE_MOUNT_POINT "/sdcard"

/* ====================================================================
 * 物理按键（BOOT）
 * ==================================================================== */

#define ESPAPERPLAY_PIN_KEY_BOOT 0          /*!< BOOT 按键（板载，按下为低电平） */
#define ESPAPERPLAY_KEY_BOOT_ACTIVE_LEVEL 0 /*!< BOOT 按键按下时的电平 */

/* ====================================================================
 * 默认总线参数
 * ==================================================================== */

/** EPD 使用的 SPI 主机（ESP32-S3：SPI2_HOST == 1，SPI3_HOST == 2）。 */
#define ESPAPERPLAY_SPI_HOST_ID 1

/** 触摸控制器使用的 I2C 端口（I2C_NUM_0 == 0）。 */
#define ESPAPERPLAY_I2C_PORT_ID 0

/** I2C 事务超时时间，单位毫秒。 */
#define ESPAPERPLAY_I2C_TIMEOUT_MS 100

#ifdef __cplusplus
}
#endif
