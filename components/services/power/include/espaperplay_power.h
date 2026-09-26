/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_power.h
 * @brief 电源管理抽象层。
 *
 * 负责设备的低功耗策略：可配置的唤醒源、自动浅睡眠管理，以及进入 ESP32
 * 深度睡眠的切换；同时提供电池 / 充电状态读取（v2.0 板：BAT_SENSE 分压
 * ADC + TP4056 CHRG#/STDBY# 回读）。v2.0 板外设电源轨 3V3_PER（EPD /
 * 触摸 / SD）由 POWER_EN 控制：board 初始化时开轨，深睡"关机"时断轨
 * （浅睡眠期间保持常通以维持触摸唤醒）。
 */

/**
 * @brief 电池供电 / 充电状态。
 */
typedef enum {
    ESPAPERPLAY_BATTERY_DISCHARGING = 0, /*!< 电池放电（无 USB 或未在充电） */
    ESPAPERPLAY_BATTERY_CHARGING,        /*!< 充电中（TP4056 CHRG# 拉低） */
    ESPAPERPLAY_BATTERY_FULL,            /*!< 充电完成（TP4056 STDBY# 拉低） */
} espaperplay_battery_state_t;

/**
 * @brief 电池状态快照。
 */
typedef struct {
    uint32_t voltage_mv;                 /*!< 电池端电压（毫伏，分压反算） */
    uint8_t percent;                     /*!< 电量百分比（0-100，电压查表估算） */
    espaperplay_battery_state_t state;   /*!< 供电 / 充电状态 */
    bool usb_present;                    /*!< USB 电源在位（充电中或充满） */
} espaperplay_battery_info_t;

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
 * @brief 睡眠期间需要联网刷新的服务描述。
 *
 * 电源服务按 @ref refresh_interval_ms 维护统一截止时间。多个服务在同一
 * 唤醒窗口到期时只恢复一次 WiFi，再依次触发并等待它们完成。所有回调均
 * 在 power_auto_sleep 任务上下文调用，不可长期持有服务内部锁。
 *
 * name 必须指向生命周期覆盖整个程序的字符串（通常为字符串常量）。
 */
typedef struct {
    const char *name;                               /*!< 日志名称，必须非空 */
    uint32_t refresh_interval_ms;                   /*!< 期望刷新粒度；过小会被全局低功耗下限钳制 */
    uint32_t refresh_timeout_ms;                    /*!< 单次等待完成的最长时间 */
    bool (*is_refresh_due)(void);                   /*!< 可选；返回 false 时本周期不联网 */
    void (*request_refresh)(void);                  /*!< 触发服务后台刷新，必须非空 */
    bool (*wait_refresh_done)(uint32_t timeout_ms); /*!< 可选；等待后台刷新完成 */
} espaperplay_sleep_refresh_service_t;

/** 睡眠期间两次 WiFi 联网窗口的最小间隔：避免服务配置过细破坏低功耗。 */
#define ESPAPERPLAY_POWER_MIN_NETWORK_WAKE_INTERVAL_MS (10U * 60U * 1000U)

/**
 * @brief 初始化电源管理。
 *
 * 建立电池电压 ADC 通道（BAT_SENSE 分压）与充电状态回读引脚
 * （CHRG#/STDBY#），并读取一次电池状态记录到日志。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_power_init(void);

/**
 * @brief 读取电池状态（电压 / 电量 / 充电状态 / USB 在位）。
 *
 * 电压经 16 次采样均值 + 出厂校准曲线换算；USB 在位由 TP4056 的
 * CHRG#/STDBY# 任一拉低推断（板上无独立 VBUS 检测脚）。可在任意任务
 * 上下文调用，单次耗时约数毫秒。
 *
 * @param[out] info 接收状态快照。
 *
 * @return 成功返回 ESP_OK；参数非法或电源服务未初始化返回相应错误码。
 */
esp_err_t espaperplay_power_get_battery_info(espaperplay_battery_info_t *info);

/**
 * @brief 配置下一次睡眠使用的唤醒源。
 *
 * @param config 唤醒源配置。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_power_configure_wakeup(const espaperplay_wakeup_config_t *config);

/**
 * @brief 进入深度睡眠（v2.0 板"关机"）。
 *
 * 时序：音频子系统下电 -> EPD 进自身深睡 -> 断开 WiFi -> 断外设电源轨
 * 3V3_PER（EPD / 触摸 / SD 随轨断电；POWER_EN 引脚深睡中悬空，由板上
 * R20 上拉保持关断）-> 以 BOOT 键（GPIO0 低电平）为唤醒源进入深度睡眠。
 *
 * 唤醒即整机重启（深睡恢复等效复位）：按 BOOT 键或按 EN 键（SW1 硬复位）
 * 重新上电启动，board 初始化时重新开轨。成功进入后本函数不会返回。
 *
 * @return 成功返回 ESP_OK（不返回），否则返回错误码。
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
 * 浅睡眠期间 GPIO 电平保持、PSRAM 自刷新；STA WiFi 会在睡前主动挂起，
 * 仅在用户唤醒或已注册网络服务到期时恢复。外设与任务状态在唤醒后完整
 * 恢复；esp_timer 高精度时间基准会被自动补偿。
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
 * @brief 注册一个睡眠期间周期联网刷新的服务（幂等，按 name 去重）。
 *
 * 注册只影响设备已经进入自动浅睡眠后的联网窗口；清醒期间仍由服务自身
 * 的后台周期负责刷新。当前固定支持最多 8 个服务，不进行堆分配。
 *
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；注册表满返回
 *         ESP_ERR_NO_MEM；同名但配置不同返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_power_register_sleep_refresh_service(
    const espaperplay_sleep_refresh_service_t *service);

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
