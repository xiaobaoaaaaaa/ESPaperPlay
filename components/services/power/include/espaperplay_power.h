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
 * @file espaperplay_power.h
 * @brief 电源管理抽象层。
 *
 * 负责设备的低功耗策略：可独立断电的电源域（EPD、触摸、SD 卡）、可配置的
 * 唤醒源，以及进入 ESP32 深度睡眠的切换。具体硬件控制（GPIO 电源轨使能、
 * esp_sleep 调用）将在后续阶段实现。
 */

/**
 * @brief 设备的独立电源域。
 */
typedef enum {
    ESPAPERPLAY_POWER_DOMAIN_EPD = 0, /*!< 电子纸面板及其驱动电源轨 */
    ESPAPERPLAY_POWER_DOMAIN_TOUCH,   /*!< 触摸控制器电源轨 */
    ESPAPERPLAY_POWER_DOMAIN_SD,      /*!< SD 卡电源轨 */
    ESPAPERPLAY_POWER_DOMAIN_MAX,
} espaperplay_power_domain_t;

/**
 * @brief 深度睡眠的唤醒源配置。
 */
typedef struct {
    bool enable_timer;          /*!< 定时器到期后唤醒 */
    uint32_t wakeup_timeout_ms; /*!< 定时器时长（毫秒，若启用） */
    bool enable_gpio;           /*!< 外部 GPIO 边沿唤醒 */
    int gpio_num;               /*!< 用作唤醒源的 GPIO */
    bool gpio_level;            /*!< 唤醒电平（true 表示高电平） */
} espaperplay_wakeup_config_t;

/**
 * @brief 初始化电源管理。
 *
 * 注册电源域电源轨并准备唤醒源 GPIO。
 *
 * @note 当前仅为骨架：尚未执行任何真实硬件控制。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_power_init(void);

/**
 * @brief 使能或关闭某个电源域。
 *
 * @param domain 需要控制的电源域。
 * @param enable true 表示上电，false 表示断电。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_power_domain_set(espaperplay_power_domain_t domain, bool enable);

/**
 * @brief 配置下一次睡眠使用的唤醒源。
 *
 * @param config 唤醒源配置。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_power_configure_wakeup(const espaperplay_wakeup_config_t *config);

/**
 * @brief 进入深度睡眠。
 *
 * 关闭已使能的电源域，并使用已配置的唤醒源进入 ESP32 深度睡眠。
 * 成功进入后本函数不会返回。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_power_enter_sleep(void);

#ifdef __cplusplus
}
#endif
