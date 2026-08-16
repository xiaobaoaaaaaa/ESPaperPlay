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
 * EPD SPI 接口（与 SD 卡共用 SPI2 主机）
 * ==================================================================== */

#define ESPAPERPLAY_PIN_EPD_SCLK 12 /*!< SPI 时钟 */
#define ESPAPERPLAY_PIN_EPD_MOSI 11 /*!< SPI MOSI */
#define ESPAPERPLAY_PIN_EPD_MISO 13 /*!< SPI MISO */
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
 * SD 卡（SPI 模式，与 EPD 共用 SPI2 主机）
 * ==================================================================== */

#define ESPAPERPLAY_PIN_SD_CS 21  /*!< SD 卡片选 */
#define ESPAPERPLAY_PIN_SD_PWR 20 /*!< SD 卡电源轨使能 */

/** SD 卡的 VFS 挂载点。 */
#define ESPAPERPLAY_STORAGE_MOUNT_POINT "/sdcard"

/** SD 卡默认 SPI 时钟频率，单位 Hz。 */
#define ESPAPERPLAY_SD_SPI_CLK_HZ 20000000

/* ====================================================================
 * 物理按键（BOOT）
 * ==================================================================== */

#define ESPAPERPLAY_PIN_KEY_BOOT 0          /*!< BOOT 按键（板载，按下为低电平） */
#define ESPAPERPLAY_KEY_BOOT_ACTIVE_LEVEL 0 /*!< BOOT 按键按下时的电平 */

/* ====================================================================
 * 默认总线参数
 * ==================================================================== */

/** EPD 与 SD 使用的 SPI 主机（ESP32-S3：SPI2_HOST == 1，SPI3_HOST == 2）。 */
#define ESPAPERPLAY_SPI_HOST_ID 1

/** 触摸控制器使用的 I2C 端口（I2C_NUM_0 == 0）。 */
#define ESPAPERPLAY_I2C_PORT_ID 0

/** I2C 事务超时时间，单位毫秒。 */
#define ESPAPERPLAY_I2C_TIMEOUT_MS 100

#ifdef __cplusplus
}
#endif
