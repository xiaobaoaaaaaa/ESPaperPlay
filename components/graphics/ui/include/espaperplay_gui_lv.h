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
 * @file espaperplay_gui_lv.h
 * @brief LVGL 移植层：LVGL 与渲染后端（espaperplay_gui）之间的桥。
 *
 * 只负责"LVGL 如何对接 ESPaperPlay"：初始化（tick / display / 绘制暂存）、
 * flush 回调链与 lv_timer_handler 渲染任务。UI 页面（widget 树）不在此层，
 * 见 espaperplay_ui.h（src/screens/）。
 */

/**
 * @brief 启动 LVGL 移植层（初始化 + 渲染任务）。
 *
 * 初始化 LVGL（lv_init + 注册 esp_timer tick 源 + 显示注册：绘制暂存 +
 * flush 回调，LVGL 的 RGB565 渲染结果经回调拷入主帧并提交脏区，周期末
 * 触发一次异步刷新），创建 lv_timer_handler 循环任务。应在
 * espaperplay_gui_init() 之后从 app_main 调用一次。
 *
 * @return 成功返回 ESP_OK；渲染后端未初始化返回其错误码；
 *         缓冲分配失败返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_gui_lv_start(void);

/**
 * @brief 在 LVGL 线程中执行一个 UI 操作（跨线程安全调用，同步等待完成）。
 *
 * LVGL 非线程安全（渲染期间跨线程 invalidate 会触发内部断言死循环），
 * 所有 UI 操作（建控件 / 改属性 / 事件回调）必须经由本函数投递到
 * gui_lvgl 任务内串行执行：本函数把回调排队，并阻塞等待其执行完成。
 * 回调内可直接调用任意 lv_* API。
 *
 * 完成信号按「调用序号」匹配：同一时刻仍只允许一个同步调用方
 * 等待（多任务并发投递需自行串行化）。
 *
 * @param fn        在 LVGL 线程执行的回调（可空，仅作唤醒测试）。
 * @param arg       透传给回调的参数。
 * @param timeout_ms 等待执行完成的超时（毫秒）。
 *
 * @return ESP_OK；移植层未启动返回 ESP_ERR_INVALID_STATE；
 *         队列满或执行超时返回 ESP_ERR_TIMEOUT。
 */
typedef void (*espaperplay_gui_lv_call_fn_t)(void *arg);
esp_err_t espaperplay_gui_lv_call(espaperplay_gui_lv_call_fn_t fn, void *arg, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
