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
 * 提供与具体控制器无关的触摸事件读取接口。GT911 通过 I2C 通信（寄存器
 * 定义与复位时序参考《GT911 Datasheet》Rev.10 及厂商 STM32 demo
 * S-GDEY075T7-FP-GT911Touch20230713）。
 *
 * 触摸数据为中断驱动：GT911 在 INT 引脚产生中断事件（触发沿随芯片配置
 * 0x804D 低 2 位而定，本面板默认下降沿），ISR 仅唤醒内部读取任务
 * （I2C 不能在 ISR 中执行），任务读取坐标后：
 *   - 若注册了事件回调（espaperplay_touch_register_event_cb），在任务
 *     上下文逐帧回调（count == 0 表示全部手指抬起）；
 *   - 同时缓存最新一帧，供 espaperplay_touch_read() 轮询读取。
 */

/**
 * @brief 单次读取可上报的最大触摸点数。
 */
#define ESPAPERPLAY_TOUCH_MAX_POINTS 5

/**
 * @brief 单个触摸点。
 */
typedef struct {
    uint16_t x;       /*!< 以显示像素为单位的 X 坐标（已换算并裁剪到有效显示区） */
    uint16_t y;       /*!< 以显示像素为单位的 Y 坐标（已换算并裁剪到有效显示区） */
    uint8_t id;       /*!< 触摸点跟踪 ID（GT911 上报，同一手指滑动期间保持不变） */
    uint8_t reserved; /*!< 保留字段，恒为 0 */
} espaperplay_touch_point_t;

/**
 * @brief 触摸帧回调。
 *
 * 由 touch 组件内部的读取任务调用（任务上下文，可执行阻塞操作）。
 * 每次 GT911 上报一帧调用一次：
 *   - count >= 1：points 为当前按下的触摸点；
 *   - count == 0：释放帧（全部手指抬起），points 内容无效。
 *
 * @param points 当前帧触摸点数组（count == 0 时不可用）。
 * @param count  当前帧触摸点数量（0 表示全部手指抬起）。
 */
typedef void (*espaperplay_touch_event_cb_t)(const espaperplay_touch_point_t *points,
                                             uint8_t count);

/**
 * @brief 初始化触摸控制器。
 *
 * 依次完成：触摸电源轨上电 → GT911 硬件复位（INT 电平锁存 I2C 地址）→
 * 初始化 I2C 主机 → 探测 GT911（回读产品 ID "911" 与配置版本/分辨率）→
 * 按芯片配置 0x804D 的 INT 触发模式配置 GPIO 中断并创建内部读取任务。
 *
 * 默认按 INT 低电平复位锁存地址 0x5D（与厂商 STM32 demo 一致）；若探测
 * 失败，自动以 INT 高电平复位锁存地址 0x14 重试一次。
 *
 * @return 成功返回 ESP_OK；两次地址探测均失败返回 ESP_ERR_NOT_FOUND；
 *         其余错误返回对应错误码。
 */
esp_err_t espaperplay_touch_init(void);

/**
 * @brief 注册触摸帧回调（覆盖已注册的回调）。
 *
 * 回调在 touch 内部读取任务上下文执行，直接调用即可；注意不要在回调中
 * 长时间阻塞（会推迟后续触摸帧的处理）。
 *
 * @param cb 回调函数；传入 NULL 取消注册。
 *
 * @return 成功返回 ESP_OK；驱动未初始化返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_touch_register_event_cb(espaperplay_touch_event_cb_t cb);

/**
 * @brief 读取最新一帧触摸点（非阻塞）。
 *
 * 返回上次读取之后内部任务缓存的最新一帧。若没有新帧（含初始化后尚未
 * 有任何触摸），*count 置 0 并返回 ESP_OK。
 *
 * @param[out] points     用于接收触摸点的缓冲区。
 * @param[in]  max_points 缓冲区容量（>= 1）。
 * @param[out] count      实际写入的触摸点数量（0 表示无新帧）。
 *
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；驱动未
 *         初始化返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_touch_read(espaperplay_touch_point_t *points, uint8_t max_points,
                                 uint8_t *count);

/**
 * @brief 打印触摸控制器诊断信息（调试用）。
 *
 * 读取并打印：INT 引脚电平、缓冲状态寄存器 0x814E（只读不消费，
 * 不影响正常读取路径）与配置刷新标志 0x8100。用于区分「芯片未扫描
 * 报点」（0x814E 一直无数据）与「芯片在报点但 INT 线异常」（0x814E
 * 有数据但 INT 电平无变化）。
 *
 * @return 成功返回 ESP_OK；驱动未初始化返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_touch_diag(void);

#ifdef __cplusplus
}
#endif
