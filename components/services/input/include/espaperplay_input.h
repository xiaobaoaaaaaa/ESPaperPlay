/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "espaperplay_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_input.h
 * @brief 输入事件管理。
 *
 * 将所有人机输入源（触摸控制器、物理按键）聚合为统一的事件流。
 * 应用程序通过 espaperplay_input_get_event() 消费事件，不直接与触摸或
 * GPIO 驱动交互。
 *
 * 内部为双队列设计（按键 / 触摸物理隔离，经 FreeRTOS Queue Set 合并
 * 消费）：
 *   - 按键队列：稀疏、事件型（单击/双击/长按等语义动作），队首投递 +
 *     满时挤最旧，按键永不丢失；
 *   - 触摸队列：高频、状态型（GT911 中断唤醒读取的坐标流，轨迹绘制
 *     需要中间点），满时丢弃新事件（顺序不乱、Queue Set 通知计数一致），
 *     触摸洪泛不挤占按键队列。
 * 触摸中断接入方式：GT911 的 I2C 读取不能在 ISR 内执行，由中断唤醒
 * 触摸任务，任务内读取坐标后经 espaperplay_input_post_event() 投递到
 * 触摸队列。
 */

/**
 * @brief 输入事件类型。
 */
typedef enum {
    ESPAPERPLAY_INPUT_EVENT_TOUCH = 0, /*!< 触摸屏事件 */
    ESPAPERPLAY_INPUT_EVENT_KEY,       /*!< 物理按键事件 */
    ESPAPERPLAY_INPUT_EVENT_MAX,
} espaperplay_input_event_type_t;

/** BOOT 按键（GPIO0）的按键标识。 */
#define ESPAPERPLAY_INPUT_KEY_ID_BOOT 0

/**
 * @brief 归一化的按键动作。
 *
 * 与底层驱动（espressif/button）的事件一一对应，屏蔽驱动细节。
 */
typedef enum {
    ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_DOWN = 0,   /*!< 按下 */
    ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_UP,         /*!< 松开 */
    ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK,     /*!< 单击（按下并松开一次） */
    ESPAPERPLAY_INPUT_KEY_ACTION_DOUBLE_CLICK,     /*!< 双击 */
    ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_START, /*!< 长按开始（达到长按阈值） */
    ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_HOLD,  /*!< 长按保持（周期性触发） */
    ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_UP,    /*!< 长按后松开 */
    ESPAPERPLAY_INPUT_KEY_ACTION_MAX,
} espaperplay_input_key_action_t;

/**
 * @brief 归一化的输入事件。
 */
typedef struct {
    espaperplay_input_event_type_t type;       /*!< 事件来源类型 */
    espaperplay_touch_point_t point;           /*!< 触摸数据（type == TOUCH 时有效） */
    uint8_t touch_pressed; /*!< 触摸是否按下（type == TOUCH 时有效；0 表示全部手指抬起） */
    uint8_t touch_points;  /*!< 本帧触摸点总数（type == TOUCH 时有效；释放帧为 0） */
    uint16_t touch_seq;    /*!< 触摸帧序号（同一帧内的各点共享，用于识别帧边界） */
    uint8_t key_id;                            /*!< 按键标识（type == KEY 时有效） */
    espaperplay_input_key_action_t key_action; /*!< 按键动作（type == KEY 时有效） */
    uint16_t key_press_time_ms;                /*!< 本次按压持续时间（type == KEY 时有效） */
} espaperplay_input_event_t;

/**
 * @brief 初始化输入子系统。
 *
 * 注册触摸控制器（通过 espaperplay_touch_*）与物理按键，并创建内部事件队列。
 *
 * 物理按键使用板载 BOOT 按键（GPIO0，按下为低电平），基于官方
 * espressif/button 组件（v4.2.0）实现：按键事件经回调归一化后投递到
 * 内部事件队列，由 espaperplay_input_get_event() 消费。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_input_init(void);

/**
 * @brief 等待下一个输入事件。
 *
 * @param[out] event      接收下一个输入事件。
 * @param[in]  timeout_ms 最长等待时间（毫秒）。0 表示非阻塞；较大的值
 *                        （接近 portMAX_DELAY）表示无限阻塞。
 *
 * @return 收到事件返回 ESP_OK，超时返回 ESP_ERR_TIMEOUT。
 */
esp_err_t espaperplay_input_get_event(espaperplay_input_event_t *event, uint32_t timeout_ms);

/**
 * @brief 获取按键动作的字符串表示（日志 / 界面显示用）。
 *
 * @param action 按键动作。
 *
 * @return 动作对应的字符串；未知动作返回 "UNKNOWN"。
 */
const char *espaperplay_input_key_action_str(espaperplay_input_key_action_t action);

/**
 * @brief 向内部事件队列注入一个事件（投递路径与真实事件相同）。
 *
 * 供测试 / 自检注入合成事件，也供后续触摸源接入时复用统一投递路径。
 * 队列满时丢弃新事件（按键回调内部走独立的高优先级投递路径，不受影响）。
 *
 * @param[in] event 要注入的事件。
 *
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；未初始化返回
 *         ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_input_post_event(const espaperplay_input_event_t *event);

#ifdef __cplusplus
}
#endif
