/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_ui.h
 * @brief UI 页面层：构建 LVGL widget 树。
 *
 * 页面函数只创建/组织 LVGL 控件并注册事件回调，不接触像素、不调用渲染
 * 后端 API（LVGL 的无效区机制自动驱动 flush 回调链）。须在
 * espaperplay_gui_lv_start() 之后调用（页面依赖 LVGL 已初始化）。
 */

/**
 * @brief 展示最小就绪演示屏：白底 + 居中标签。
 */
void espaperplay_ui_demo_show(void);

/**
 * @brief 展示局部刷新压力测试屏。
 *
 * 多区域以不同节奏变化（200ms 小数字快刷 / 进度条 / 2s 大黑块位移触发
 * 大面积刷新），配合 worker 日志验证：异步刷新不阻塞 LVGL（状态行实测
 * 周期稳定）、脏区只刷小窗（worker 日志区域尺寸变化）。
 */
void espaperplay_ui_test_show(void);

/**
 * @brief 展示 LVGL 自带 benchmark 测试屏。
 *
 * 连续多场景高速全屏/局部重绘（动画、文字、图形、图像），制造高频脏区
 * 提交与排队合并，用于在高压下暴露并验证异步刷新链路（快照合并 + worker
 * 局刷）的竞态问题。须在 espaperplay_gui_lv_start() 之后调用。
 */
void espaperplay_ui_benchmark_show(void);

/**
 * @brief 展示主界面（最基础原型：标题 + 运行时间 + 状态行 + 占位提示）。
 *
 * 等价于 espaperplay_ui_page_push(&espaperplay_ui_page_home)。
 */
void espaperplay_ui_home_show(void);

/**
 * @brief 页面描述（进入/退出钩子，均在 LVGL 线程内执行）。
 *
 * enter：构建页面内容到当前屏幕（屏幕已由页面栈清空），并创建页面级
 *       资源（定时器等）；exit：释放页面级资源（删除定时器等），可为 NULL。
 */
typedef struct {
    void (*enter)(void); /*!< 进入页面：构建内容（LVGL 线程内） */
    void (*exit)(void);  /*!< 退出页面：清理资源（LVGL 线程内，可 NULL） */
} espaperplay_ui_page_t;

#define ESPAPERPLAY_UI_PAGE_MAX 8 /*!< 页面栈最大深度 */

/**
 * @brief 压入并进入一个页面（栈管理）。
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
 * @brief 退出当前页并重建上一页（根页面不可弹出）。
 *
 * @return ESP_OK；栈中无页面可弹（已是根）返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t espaperplay_ui_page_pop(void);

/**
 * @brief 当前页面栈深度（1 = 仅根页面）。
 */
uint8_t espaperplay_ui_page_depth(void);

/** 主界面页面实例（screen_home.c）。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_home;

#ifdef __cplusplus
}
#endif
