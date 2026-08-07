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
 */

/**
 * @brief 输入事件类型。
 */
typedef enum {
    ESPAPERPLAY_INPUT_EVENT_TOUCH = 0, /*!< 触摸屏事件 */
    ESPAPERPLAY_INPUT_EVENT_KEY,       /*!< 物理按键事件 */
    ESPAPERPLAY_INPUT_EVENT_MAX,
} espaperplay_input_event_type_t;

/**
 * @brief 归一化的输入事件。
 */
typedef struct {
    espaperplay_input_event_type_t type; /*!< 事件来源类型 */
    espaperplay_touch_point_t point;     /*!< 触摸数据（type == TOUCH 时有效） */
    uint8_t key_id;                      /*!< 按键标识（type == KEY 时有效） */
} espaperplay_input_event_t;

/**
 * @brief 初始化输入子系统。
 *
 * 注册触摸控制器（通过 espaperplay_touch_*）与物理按键，并创建内部事件队列。
 *
 * @note 当前仅为骨架。
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

#ifdef __cplusplus
}
#endif
