/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_touch.h
 * @brief 电容触摸屏（GT911）抽象层。
 *
 * 提供与具体控制器无关的触摸事件读取接口，GT911 通过 I2C 通信。
 * 具体的 GT911 寄存器驱动尚未实现，将在后续阶段接入这些 API。
 */

/**
 * @brief 单次读取可上报的最大触摸点数。
 */
#define ESPAPERPLAY_TOUCH_MAX_POINTS 5

/**
 * @brief 单个触摸点。
 */
typedef struct {
    uint16_t x;       /*!< 以显示像素为单位的 X 坐标 */
    uint16_t y;       /*!< 以显示像素为单位的 Y 坐标 */
    uint8_t id;       /*!< 触摸点跟踪 ID */
    uint8_t reserved; /*!< 保留字段，恒为 0 */
} espaperplay_touch_point_t;

/**
 * @brief 初始化触摸控制器。
 *
 * 配置 I2C 总线、复位 GT911 并回读其能力寄存器。
 * 触摸中断引脚保持使能，供后续使用。
 *
 * @note 当前仅为骨架：GT911 寄存器驱动尚未实现。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_touch_init(void);

/**
 * @brief 读取所有待处理的触摸点（非阻塞）。
 *
 * @param[out] points     用于接收触摸点的缓冲区。
 * @param[in]  max_points 缓冲区容量（>= 1）。
 * @param[out] count      实际写入的触摸点数量。
 *
 * @return 成功返回 ESP_OK；驱动尚未实现时返回 ESP_ERR_NOT_SUPPORTED；
 *         否则返回错误码。
 */
esp_err_t espaperplay_touch_read(espaperplay_touch_point_t *points, uint8_t max_points,
                                 uint8_t *count);

#ifdef __cplusplus
}
#endif
