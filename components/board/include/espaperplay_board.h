/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

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
 * @brief 初始化板级资源：外设电源轨、GPIO、SPI 与 I2C 总线。
 *
 * 执行任何外设组件运行前所需的板级初始化。第一步即接通外设电源轨
 * 3V3_PER（v2.0 板：TOUCH / EPD / SD 供电由 POWER_EN 控制，复位后默认
 * 关断，见 espaperplay_config.h），等轨稳定后再配置其余资源。SD 卡走
 * 独立 SDMMC/SDIO 主机，由 sd 驱动组件自行初始化；I2C 主机（GT911）
 * 由 touch 组件自建。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_board_init(void);

/**
 * @brief 接通 / 关断外设电源轨 3V3_PER（POWER_EN，低电平开轨）。
 *
 * 轨上挂 TOUCH / EPD / SD 三类外设：关轨前调用方须确保各驱动已进入
 * 安全状态（EPD 已睡、无 SD 访问）。开轨后内置稳定等待。深睡关机路径
 * 由 power 服务调用本函数断轨；正常运行期轨道保持常通（浅睡眠不断轨，
 * 触摸唤醒依赖轨上供电）。
 *
 * @param on true=开轨，false=关轨。
 */
void espaperplay_board_peripheral_rail_set(bool on);

/** 外设电源轨当前是否处于接通状态（上电默认关，board_init 后为开）。 */
bool espaperplay_board_peripheral_rail_is_on(void);

#ifdef __cplusplus
}
#endif
