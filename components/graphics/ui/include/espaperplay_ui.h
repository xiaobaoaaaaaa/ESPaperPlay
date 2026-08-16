/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

#include "espaperplay_input.h"

/**
 * @file espaperplay_ui.h
 * @brief UI 页面层：构建 LVGL widget 树。
 *
 * 页面函数只创建/组织 LVGL 控件并注册事件回调，不接触像素、不调用渲染
 * 后端 API（LVGL 的无效区机制自动驱动 flush 回调链）。须在
 * espaperplay_gui_lv_start() 之后调用（页面依赖 LVGL 已初始化）。
 */

/**
 * @brief 按键输入自检（验收用，默认关闭）。
 *
 * 使能后 espaperplay_ui_key_input_start() 会额外创建自检任务：注入合成
 * 按键事件（与真实按键走同一条 input 队列 / 分发路径），验证
 * "input 队列 -> GUI 读取 -> 页面响应"整条链路：单击进入测试页
 * （页面栈深度 1 -> 2），长按松开返回（2 -> 1），往返后再进入（1 -> 2），
 * 结果以日志输出（PASS / FAIL）。
 */
#ifndef ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST
#define ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST 0
#endif

/**
 * @brief 展示主界面（最基础原型：标题 + NTP 系统时钟 + 状态行 + 占位提示）。
 *
 * 等价于 espaperplay_ui_page_push(&espaperplay_ui_page_home)。
 */
void espaperplay_ui_home_show(void);

/**
 * @brief 页面描述（进入/退出/按键/触摸钩子，均在 LVGL 线程内执行）。
 *
 * 按键与触摸事件均经输入分发任务转发（见 espaperplay_ui_key_input_start）：
 *   - enter：构建页面内容到当前屏幕（屏幕已由页面栈清空），并创建页面级
 *     资源（定时器等）；exit：释放页面级资源（删除定时器等），可为 NULL；
 *   - on_key：页面级按键处理（按键分发任务把按键事件转发给栈顶页面的该
 *     钩子，页面据此更新自身内容或发起导航），可为 NULL；
 *   - on_touch：页面级触摸处理（触摸事件经输入分发任务按 ~30ms 窗口批量
 *     投递后逐条转发给栈顶页面的该钩子，页面据此展示触摸轨迹/坐标；
 *     LVGL 控件点击不依赖本钩子，由触摸指针 indev 直接驱动），可为 NULL。
 */
typedef struct {
    void (*enter)(void); /*!< 进入页面：构建内容（LVGL 线程内） */
    void (*exit)(void);  /*!< 退出页面：清理资源（LVGL 线程内，可 NULL） */
    void (*on_key)(const espaperplay_input_event_t *event);   /*!< 按键事件（LVGL 线程内，可 NULL） */
    void (*on_touch)(const espaperplay_input_event_t *event); /*!< 触摸事件（LVGL 线程内，可 NULL） */
} espaperplay_ui_page_t;

#define ESPAPERPLAY_UI_PAGE_MAX 8 /*!< 页面栈最大深度 */

/**
 * @brief 压入并进入一个页面（栈管理，跨线程安全）。
 *
 * 先调用当前页 exit 清理，再清空屏幕并调用新页 enter 构建。全部在
 * LVGL 线程内执行（内部经 espaperplay_gui_lv_call 投递，同步等待完成）。
 *
 * @param page 页面描述（enter 必须非 NULL；调用方须保持有效直到返回）。
 *
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；栈满返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_ui_page_push(const espaperplay_ui_page_t *page);

/**
 * @brief 退出当前页并重建上一页（根页面不可弹出，跨线程安全）。
 *
 * @return ESP_OK；栈中无页面可弹（已是根）返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t espaperplay_ui_page_pop(void);

/**
 * @brief 压入并进入一个页面（LVGL 线程内直接切换，不做跨线程投递）。
 *
 * 供页面钩子 / 按键分发等已在 LVGL 线程内的代码使用：若再调用
 * espaperplay_ui_page_push() 会经 gui_lv_call 投递并阻塞等待自身，死锁。
 *
 * @param page 页面描述（enter 必须非 NULL）。
 *
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；栈满返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_ui_page_push_lv(const espaperplay_ui_page_t *page);

/**
 * @brief 退出当前页并重建上一页（LVGL 线程内直接切换，不做跨线程投递）。
 *
 * @return ESP_OK；栈中无页面可弹（已是根）返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t espaperplay_ui_page_pop_lv(void);

/**
 * @brief 把按键事件转发给栈顶页面的 on_key 钩子（须在 LVGL 线程内调用）。
 *
 * 由按键分发任务调用；栈为空或无钩子时为空操作。
 *
 * @param event 按键事件。
 */
void espaperplay_ui_page_handle_key_lv(const espaperplay_input_event_t *event);

/**
 * @brief 把触摸事件转发给栈顶页面的 on_touch 钩子（须在 LVGL 线程内调用）。
 *
 * 由输入分发任务调用（触摸事件按 ~30ms/32 点批量投递，批内逐条转发，
 * 保留全部中间坐标点）；栈为空或无钩子时为空操作。
 *
 * @param event 触摸事件。
 */
void espaperplay_ui_page_handle_touch_lv(const espaperplay_input_event_t *event);

/**
 * @brief 启动输入分发任务（input 队列 -> LVGL 线程 -> 页面钩子 / 触摸指针）。
 *
 * 任务阻塞在 espaperplay_input_get_event() 上（按键与触摸事件走同一条
 * 合并队列）：
 *   - 按键事件经 espaperplay_gui_lv_call 投递到 LVGL 线程，由
 *     espaperplay_ui_page_handle_key_lv() 转发给当前页面 on_key（导航 /
 *     刷新内容）；
 *   - 触摸事件直接更新 LVGL 指针 indev 状态（espaperplay_ui_touch_update，
 *     任意线程安全），并按 ~30ms 窗口批量投递、逐条转发给当前页面
 *     on_touch（轨迹绘制需要全部中间坐标点）。
 * 须在 espaperplay_input_init()、espaperplay_gui_lv_start() 之后调用。
 *
 * ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST=1 时，任务启动后先执行按键链路自检
 * （注入合成事件并断言页面栈响应），结果以日志输出。
 *
 * @return ESP_OK；任务创建失败返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_ui_key_input_start(void);

/**
 * @brief 当前页面栈深度（1 = 仅根页面）。
 */
uint8_t espaperplay_ui_page_depth(void);

/** 主界面页面实例（screen_home.c）。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_home;

/** 测试页页面实例（screen_test.c）：局刷压力测试 + 按键/触摸事件显示 + 可点击返回按钮，双击旋转屏幕，长按返回。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_test;

/** 天气页页面实例（screen_weather.c）：展示和风天气快照（实时 / 3 日预报 / 空气 / 天文 / 降水 / 预警），单击返回。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_weather;

#ifdef __cplusplus
}
#endif
