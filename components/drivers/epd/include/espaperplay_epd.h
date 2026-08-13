/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "espaperplay_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_epd.h
 * @brief 电子纸显示屏（EPD）抽象层（GDEY075T7-T01 / UC8179）。
 *
 * 本组件实现 GDEY075T7-T01（7.5"，800x480，UC8179 控制器，SPI 接口）驱动：
 * SPI 设备注册、硬件复位、面板初始化（PSR / PON / CDI 等）、全屏与局部
 * （Partial Window）刷新、深度睡眠与电源轨关断。
 *
 * 实现依据：
 *  - 《GDEY075T7-T01 规格书》第 7 章命令表（MinerU 转换版 Markdown 在仓库根目录）；
 *  - 厂商随板参考工程 S-GDEY075T7-FP-GT911Touch20230713（HARDWARE/EPD/
 *    Display_EPD_W21.c，UC8179 时序已上板验证）。
 *
 * 关键行为约定：
 *  - 像素极性：数据位 1 = 白（0xFF），0 = 黑（0x00）——与参考工程 GUI_Paint.h
 *    的 WHITE=0xFF / BLACK=0x00 一致（KW 模式，CDI DDX=01）；
 *  - 全屏刷新：DTM1（旧图像）写 0xFF（近似"旧=全白"，让每个黑像素都经历
 *    白->黑翻转，最大限度清除上一帧残影），DTM2（新图像）写用户数据；
 *  - 局部刷新：CDI(0xA9,0x07) -> PTIN(0x91) -> PTL(0x90, 窗口) -> DTM2(0x13)
 *    -> DRF(0x12) -> 等待 BUSY -> PTOUT(0x92)。x 与 width 必须为 8 的倍数
 *    （1bpp 按字节寻址）；
 *  - 每次刷新前都会重新初始化控制器（硬件复位 + PON），因此「睡眠唤醒后
 *    无需额外调用 init」；刷新完成后面板保持上电，由调用方决定何时调用
 *    espaperplay_epd_sleep() 进入低功耗（参考工程提示：刷新后应进入睡眠）。
 *
 * 硬件引脚与 SPI 参数集中在 espaperplay_config.h。EPD 与 SD 卡共用 SPI2
 * 主机，主机总线由 board 组件初始化；本组件仅注册自己的 SPI 设备，并自行
 * 管理 DC / RST / BUSY / PWR 引脚（与 touch 组件的做法一致）。
 */

/**
 * @brief EPD 刷新模式。
 */
typedef enum {
    ESPAPERPLAY_EPD_MODE_FULL = 0, /*!< 全屏刷新（较慢，对比度更高，可清除残影） */
    ESPAPERPLAY_EPD_MODE_PARTIAL,  /*!< 局部 / 快速刷新（屏幕不闪烁，区域 8 像素对齐） */
    ESPAPERPLAY_EPD_MODE_MAX,
} espaperplay_epd_mode_t;

/**
 * @brief 启动自检任务（显示测试图案）。
 *
 * 使能后，espaperplay_epd_init() 会创建一个后台任务，依次执行：全屏清白、
 * 全屏测试图案（左半屏黑）、局部刷新测试（黑色区域内翻转白色方块），
 * 用于上电自检 / 验收；完成后自动进入睡眠并转为空闲轮询。
 * 接入正式 UI 后应置 0 关闭。
 */
#ifndef ESPAPERPLAY_EPD_ENABLE_SELFTEST
#define ESPAPERPLAY_EPD_ENABLE_SELFTEST 1
#endif

/**
 * @brief 初始化时是否使能 EPD 电源轨（ESPAPERPLAY_PIN_EPD_PWR 拉高）。
 */
#ifndef ESPAPERPLAY_EPD_ENABLE_POWER_PIN
#define ESPAPERPLAY_EPD_ENABLE_POWER_PIN 1
#endif

/**
 * @brief 等待 BUSY 释放的超时时间（毫秒）。
 *
 * 全屏刷新典型耗时约 2~3s（规格书未给出精确值），此处取 10s 作为安全上限；
 * 超时返回 ESP_ERR_TIMEOUT。
 */
#ifndef ESPAPERPLAY_EPD_BUSY_TIMEOUT_MS
#define ESPAPERPLAY_EPD_BUSY_TIMEOUT_MS 10000
#endif

/**
 * @brief 初始化电子纸显示屏。
 *
 * 完成电源轨上电、DC/RST/BUSY GPIO 配置、SPI 设备注册与硬件复位，随后使
 * 面板进入深度睡眠（低功耗待机）。第一次调用 espaperplay_epd_refresh()
 * 时会自动完成控制器初始化（PSR/PON），无需额外步骤。
 *
 * @note 本函数可安全重复调用；esp_epd_power_off() 之后再次调用可重新上电。
 *
 * @return 成功返回 ESP_OK；SPI 设备注册或硬件初始化失败返回相应错误码。
 */
esp_err_t espaperplay_epd_init(void);

/**
 * @brief 刷新电子纸显示屏。
 *
 * @param image_buf 图像缓冲指针（1 bpp，左上角为原点，数据位 1=白 / 0=黑）。
 *                   传 NULL 表示执行"清屏（全白）"刷新。
 *                   全屏模式：必须为整帧（800x480/8 = 48000 字节）；
 *                   局部模式：必须为窗口（width*height/8 字节）。
 * @param x          区域左上角 X 坐标（仅局部模式使用，须为 8 的倍数）。
 * @param y          区域左上角 Y 坐标（仅局部模式使用）。
 * @param width      区域宽度（像素；仅局部模式使用，须为 8 的倍数）。
 * @param height     区域高度（像素；仅局部模式使用）。
 * @param mode       刷新模式（全屏或局部）。
 *
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；BUSY 超时返回
 *         ESP_ERR_TIMEOUT；其余底层错误返回相应错误码。
 */
esp_err_t espaperplay_epd_refresh(const void *image_buf, uint16_t x, uint16_t y, uint16_t width,
                                  uint16_t height, espaperplay_epd_mode_t mode);

/**
 * @brief 使电子纸显示屏进入深度睡眠。
 *
 * 依次执行：CDI 睡眠设置 -> POF（关断内部电源，等待 BUSY）-> DSLP(0xA5)。
 * 面板保留当前图像；再次刷新时驱动会自动硬件复位唤醒并重新初始化。
 *
 * @return 成功返回 ESP_OK；BUSY 超时返回 ESP_ERR_TIMEOUT；未初始化返回
 *         ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_epd_sleep(void);

/**
 * @brief 完全关闭电子纸显示屏的电源轨。
 *
 * 若面板尚处于上电状态，先执行深度睡眠，再拉低 ESPAPERPLAY_PIN_EPD_PWR。
 * 再次使用前需调用 espaperplay_epd_init() 重新上电。
 *
 * @return 成功返回 ESP_OK，否则返回相应错误码。
 */
esp_err_t espaperplay_epd_power_off(void);

#ifdef __cplusplus
}
#endif
