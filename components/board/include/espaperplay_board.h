/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_board.h
 * @brief ESPaperPlay 板级硬件抽象。
 *
 * board 组件负责物理硬件映射：GPIO 引脚定义（见 espaperplay_config.h）、
 * SPI 与 I2C 总线配置，以及各外设总线的一次性初始化。
 * 上层组件（epd、touch、storage、power）使用本组件暴露的引脚 / 总线定义，
 * 而不是在自身代码中硬编码硬件细节。
 */

/**
 * @brief 初始化板级资源：GPIO、SPI 与 I2C 总线。
 *
 * 执行任何外设组件运行前所需的板级初始化，包括引脚复用、SPI 主机
 * （EPD + SD）与 I2C 主机（GT911）配置。
 *
 * @note 当前骨架阶段仅实现日志 / 上电钩子；实际的总线初始化将在首个
 *       具体外设驱动实现时补充。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_board_init(void);

#ifdef __cplusplus
}
#endif
