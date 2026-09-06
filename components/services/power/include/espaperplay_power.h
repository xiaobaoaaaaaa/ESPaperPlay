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
 * 负责设备的低功耗策略：可配置的唤醒源、自动浅睡眠管理，以及进入 ESP32
 * 深度睡眠的切换。TOUCH / EPD / SD 三个部件的供电为常供电，板上无 MOS
 * 电源轨、固件不做独立断电（历史电源轨配置已移除）。
 */

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
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_power_init(void);

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
 * 使用已配置的唤醒源进入 ESP32 深度睡眠。
 * 成功进入后本函数不会返回。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_power_enter_sleep(void);

/**
 * @brief 进入浅睡眠（light sleep）。
 *
 * 在已通过 espaperplay_power_configure_wakeup() 配置唤醒源的前提下，
 * 先确保电子纸面板已进入自身深度睡眠（避免睡眠期间有在途 SPI 传输），
 * 再调用 esp_light_sleep_start() 进入浅睡眠。唤醒源（触摸 INT / BOOT
 * 按键 / UART）触发后本函数返回。
 *
 * 浅睡眠期间 GPIO 电平保持、PSRAM 自刷新、WiFi 保持关联，外设与任务
 * 状态在唤醒后完整恢复；esp_timer 高精度时间基准会被自动补偿。
 *
 * @note 必须在任务上下文调用（不可在 ISR 中调用）。
 *
 * @return 正常入睡并唤醒返回 ESP_OK；触摸 INT 仍处有效电平、本轮主动
 *         放弃入睡（由调用方稍后重试）返回 ESP_ERR_INVALID_STATE；其余
 *         错误返回对应错误码。
 */
esp_err_t espaperplay_power_enter_light_sleep(void);

/**
 * @brief 设置设备自动浅睡眠超时（毫秒）。
 *
 * 超过该时长无任何用户操作（按键 / 触摸）则自动进入浅睡眠。
 * 设为 0 关闭自动睡眠（仅手动调用 espaperplay_power_enter_light_sleep()
 * 时才会睡眠）。
 *
 * @param[in] timeout_ms 超时（毫秒，0=关闭）。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t espaperplay_power_set_auto_sleep_timeout_ms(uint32_t timeout_ms);

/**
 * @brief 启动自动浅睡眠管理任务。
 *
 * 创建并启动后台任务：周期性检查用户活动时间戳，超时即进入浅睡眠。
 * 须在 espaperplay_power_configure_wakeup() 之后、输入与 EPD 子系统
 * 初始化完成后调用。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_power_start_auto_sleep(void);

/**
 * @brief 设置周期定时器唤醒间隔（毫秒）。
 *
 * 用于睡眠期间周期唤醒以刷新内容（如主界面更新时钟）。每次进入浅睡眠前
 * 会按本间隔装载 RTC 定时器唤醒源（一次性，唤醒后自动重设）。
 *
 * 设为 0 关闭周期唤醒（仅由用户操作 / 串口唤醒）。典型用法：进入主界面
 * 时设为 60000（每分钟唤醒更新时钟），离开主界面时设回 0。
 *
 * @param[in] timeout_ms 间隔（毫秒，0=关闭）。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t espaperplay_power_set_periodic_wakeup_ms(uint32_t timeout_ms);

/**
 * @brief 启用/关闭周期唤醒的"分钟对齐"模式。
 *
 * 启用后，每次进入浅睡眠前会计算到下一分钟边界的剩余时间作为定时器唤醒
 * 间隔（提前约 500ms 唤醒），使唤醒恰好落在分钟切换点附近。主界面时钟
 * 因此能在分钟更新时立即刷新，而非固定相位导致显示滞后真实分钟达 ~60s。
 *
 * 与 espaperplay_power_set_periodic_wakeup_ms() 互斥：启用本模式时忽略
 * 固定间隔，按分钟边界计算唤醒时刻。
 *
 * @param[in] enable true=启用分钟对齐，false=关闭。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t espaperplay_power_set_periodic_wakeup_minute_aligned(bool enable);

/**
 * @brief 记录一次外部活动（任意线程可调用）。
 *
 * 供输入子系统之外的活动源使用，当前为 Web 控制台心跳：管理页面打开时
 * 前端周期调用 POST /api/heartbeat，服务端转调本函数。自动睡眠管理任务
 * 在最近一次外部活动后的保持唤醒窗口内（70s，覆盖浏览器后台标签页的
 * 定时器节流）不进入浅睡眠；窗口内心跳持续到达即持续唤醒，客户端关闭 /
 * 断网后设备在窗口超时后自然恢复自动睡眠。
 */
void espaperplay_power_note_external_activity(void);

#ifdef __cplusplus
}
#endif
