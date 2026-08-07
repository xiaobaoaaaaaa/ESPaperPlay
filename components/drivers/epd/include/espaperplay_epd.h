/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"

#include "espaperplay_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_epd.h
 * @brief 电子纸显示屏（EPD）抽象层。
 *
 * 本组件定义与具体控制器无关的电子纸 API。具体驱动（例如 GDEY075T7-T01
 * 的 UC8179）有意*尚未*实现，将在后续阶段接入这些接口。屏幕几何参数
 * 取自 espaperplay_config.h。
 */

/**
 * @brief EPD 刷新模式。
 */
typedef enum {
    ESPAPERPLAY_EPD_MODE_FULL = 0, /*!< 全屏刷新（较慢，对比度更高） */
    ESPAPERPLAY_EPD_MODE_PARTIAL,  /*!< 局部 / 快速刷新 */
    ESPAPERPLAY_EPD_MODE_MAX,
} espaperplay_epd_mode_t;

/**
 * @brief 初始化电子纸显示屏。
 *
 * 准备 EPD 电源轨与 SPI 接口，执行硬件复位，并使面板进入低功耗状态。
 *
 * @note 当前仅为骨架：UC8179 驱动尚未实现。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_epd_init(void);

/**
 * @brief 刷新电子纸显示屏的指定区域。
 *
 * @param image_buf  区域像素缓冲指针（1 bpp，左上角为原点）。
 *                   传 NULL 表示仅执行"清屏"刷新。
 * @param x          区域左上角 X 坐标。
 * @param y          区域左上角 Y 坐标。
 * @param width      区域宽度（像素）。
 * @param height     区域高度（像素）。
 * @param mode       刷新模式（全屏或局部）。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_epd_refresh(const void *image_buf, uint16_t x, uint16_t y, uint16_t width,
                                  uint16_t height, espaperplay_epd_mode_t mode);

/**
 * @brief 使电子纸显示屏进入深度睡眠。
 *
 * 关闭面板（可选地连同其电源轨）。再次使用前需调用 espaperplay_epd_init()
 * 唤醒。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_epd_sleep(void);

/**
 * @brief 完全关闭电子纸显示屏的电源轨。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_epd_power_off(void);

#ifdef __cplusplus
}
#endif
