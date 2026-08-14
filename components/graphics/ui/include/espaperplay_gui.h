/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "espaperplay_epd.h"
#include "espaperplay_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_gui.h
 * @brief GUI 抽象层（RGB565 主帧 + 模式化转换级）。
 *
 * 渲染后端架构：
 *   - 单一 RGB565 主帧缓冲（800x480，750KB，PSRAM）：LVGL / 阅读器统一
 *     渲染目标（后续 LVGL 配 LV_COLOR_DEPTH=16 直接画入）；
 *   - flush 时按当前模式把 RGB 转换为 e-paper 帧格式：
 *       · 交互模式（BW）：RGB -> 1bpp，脏区转换 + 局部刷新（~370ms），
 *         转换器可选快速阈值或 Bayer 4x4 有序抖动（默认，纯黑/纯白
 *         位精确，零伪影）；
 *       · 高清模式（GRAY4）：RGB -> 2bpp 四灰阶，默认 Floyd–Steinberg
 *         误差扩散抖动（质量最好；Bayer 4x4 为快速选项），整帧转换 +
 *         全屏刷新（~2.5s，仅全屏）；
 *   - 模式切换是页面级操作（espaperplay_gui_show_ui / show_image），
 *     切换后主帧内容不变，只是转换路径不同；驱动保证切换刷新干净
 *     （灰阶->黑白旧平面反相，清除中间灰残留）。
 *
 * 典型流程：
 *   1. espaperplay_gui_get_framebuffer() 取得 RGB565 渲染目标；
 *   2. 渲染器绘制（RGB565，小端字节序）；
 *   3. espaperplay_gui_submit_area() 标记变更区域（一帧内可多次，自动
 *      8 像素对齐并合并包围盒）；
 *   4. espaperplay_gui_flush() 按模式转换 + 刷新。
 *
 * 线程模型：渲染后端不是线程安全的，应由单一渲染任务调用；epd 驱动内部
 * 自带互斥锁，多任务并发刷新仍然安全。
 */

/**
 * @brief 刷新模式（决定 flush 时的转换路径与驱动模式）。
 */
typedef enum {
    ESPAPERPLAY_GUI_COLOR_BW = 0, /*!< 交互模式：RGB->1bpp，脏区局部刷新 */
    ESPAPERPLAY_GUI_COLOR_GRAY4,  /*!< 高清模式：RGB->2bpp 四灰阶，全屏刷新 */
    ESPAPERPLAY_GUI_COLOR_MAX,
} espaperplay_gui_color_t;

/**
 * @brief 1bpp 转换器。
 */
typedef enum {
    ESPAPERPLAY_GUI_CONVERTER_THRESHOLD = 0, /*!< 快速阈值（L>=128 白），无抖动，速度快 */
    ESPAPERPLAY_GUI_CONVERTER_BAYER,         /*!< Bayer 4x4 有序抖动（默认）：中间灰呈点阵，
                                              纯黑/纯白位精确（0/255 直通，无伪影） */
    ESPAPERPLAY_GUI_CONVERTER_MAX,
} espaperplay_gui_converter_t;

/**
 * @brief 四灰阶抖动算法（高清模式）。
 *
 * FS（默认）：Floyd–Steinberg 误差扩散，质量最好（照片质感、无点阵纹理），
 * 但误差逐行依赖，只适合整帧转换（脏区局部转换会在区域边界产生伪影——
 * 灰阶模式本来就是全屏刷新，不受影响）。性能约 50ms/整帧（实测见自检）。
 * BAYER：4x4 有序抖动，快（约 10ms），质量略差（4px 周期纹理）。
 */
typedef enum {
    ESPAPERPLAY_GUI_GRAY4_DITHER_FS = 0, /*!< Floyd–Steinberg 误差扩散（默认，质量最好） */
    ESPAPERPLAY_GUI_GRAY4_DITHER_BAYER,  /*!< Bayer 4x4 偏置抖动（快速） */
    ESPAPERPLAY_GUI_GRAY4_DITHER_MAX,
} espaperplay_gui_gray4_dither_t;

/**
 * @brief 渲染帧缓冲描述（RGB565）。
 */
typedef struct {
    uint8_t *buffer;               /*!< RGB565 帧缓冲基址（PSRAM），小端字节序 */
    uint16_t width;                /*!< 屏幕宽度（像素） */
    uint16_t height;               /*!< 屏幕高度（像素） */
    uint16_t stride;               /*!< 每行字节数（= width*2 = 1600） */
    espaperplay_gui_color_t color; /*!< 当前刷新模式 */
} espaperplay_gui_framebuffer_t;

/**
 * @brief 启动渲染后端自检任务（验收用，默认关闭）。
 *
 * 使能后 espaperplay_gui_init() 会创建一个后台任务，依次验证：纯黑白 UI
 * 位精确性（Bayer 无伪影）、灰度渐变（Bayer 平滑点阵）、阈值对照、
 * gray4 高清全屏、模式切换无残留，最后恢复黑白并清白。
 */
#ifndef ESPAPERPLAY_GUI_ENABLE_SELFTEST
#define ESPAPERPLAY_GUI_ENABLE_SELFTEST 1
#endif

/**
 * @brief 脏区面积达到该像素数时改用全屏刷新（而非局部）。
 */
#ifndef ESPAPERPLAY_GUI_FULL_AREA_THRESHOLD_PIXELS
#define ESPAPERPLAY_GUI_FULL_AREA_THRESHOLD_PIXELS                                                 \
    (ESPAPERPLAY_DISPLAY_WIDTH * ESPAPERPLAY_DISPLAY_HEIGHT / 2)
#endif

/**
 * @brief 初始化 GUI 子系统（渲染后端）。
 *
 * 分配 RGB565 主帧（PSRAM，750KB）与 1bpp/2bpp 转换输出暂存（PSRAM 优先，
 * 失败回退内部 RAM）。应在 espaperplay_epd_init() 之后调用。
 *
 * @return 成功返回 ESP_OK；缓冲分配失败返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_gui_init(void);

/**
 * @brief 启动 GUI 任务。
 *
 * 创建 GUI 渲染 / 事件循环任务（后续阶段为 LVGL 任务），当前未实现。
 *
 * @return 当前返回 ESP_ERR_NOT_SUPPORTED（LVGL 适配阶段实现）。
 */
esp_err_t espaperplay_gui_start(void);

/**
 * @brief 切换刷新模式（BW 交互 / GRAY4 高清）。
 *
 * 只改变 flush 的转换路径与驱动模式，不触发刷新、不清空主帧。切换后请
 * 重新提交全屏区域并 flush（或直接调用 show_ui / show_image）。
 *
 * @param color 目标模式。
 *
 * @return 成功返回 ESP_OK；未初始化返回 ESP_ERR_INVALID_STATE；参数非法
 *         返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t espaperplay_gui_set_color(espaperplay_gui_color_t color);

/**
 * @brief 选择 1bpp 转换器（阈值或 Bayer 抖动）。
 *
 * 仅影响 BW 模式的转换；gray4 模式抖动算法由 set_gray4_dither 决定。
 * 默认 BAYER。
 *
 * @param converter 转换器。
 *
 * @return 成功返回 ESP_OK；未初始化或参数非法返回相应错误码。
 */
esp_err_t espaperplay_gui_set_converter(espaperplay_gui_converter_t converter);

/**
 * @brief 选择四灰阶抖动算法（FS 误差扩散 / Bayer 有序抖动）。
 *
 * 默认 FS（质量最好）；FS 仅用于整帧转换（灰阶恒为全屏刷新，无脏区限制）。
 *
 * @param dither 抖动算法。
 *
 * @return 成功返回 ESP_OK；未初始化或参数非法返回相应错误码。
 */
esp_err_t espaperplay_gui_set_gray4_dither(espaperplay_gui_gray4_dither_t dither);

/**
 * @brief 获取 RGB565 主帧缓冲。
 *
 * @param[out] fb 帧缓冲描述（buffer 由本组件持有，渲染器直接写入，
 *                勿自行释放）。
 *
 * @return 成功返回 ESP_OK；未初始化或参数为空返回相应错误码。
 */
esp_err_t espaperplay_gui_get_framebuffer(espaperplay_gui_framebuffer_t *fb);

/**
 * @brief 标记一个已变更区域（脏区）。
 *
 * 区域自动裁剪到屏内、X 方向 8 像素对齐，并与本帧已提交的脏区合并为
 * 包围盒。一帧内可多次调用，flush() 时合并为一次刷新。
 *
 * @return 成功返回 ESP_OK；未初始化返回 ESP_ERR_INVALID_STATE；空区域
 *         忽略并返回 ESP_OK。
 */
esp_err_t espaperplay_gui_submit_area(uint16_t x, uint16_t y, uint16_t width, uint16_t height);

/**
 * @brief 执行一次合并后的转换 + 刷新。
 *
 * BW 模式：脏区面积达阈值用整帧转换 + 全屏刷新，否则只转换脏区（输出即
 * 窗口打包行）做局部刷新；GRAY4 模式：整帧转换 + 全屏刷新（驱动约束）。
 *
 * @return 成功返回 ESP_OK；无脏区返回 ESP_OK（空操作）；失败返回相应
 *         错误码（脏区保留，可重试）。
 */
esp_err_t espaperplay_gui_flush(void);

/**
 * @brief 切回交互模式并整帧刷新（页面级操作）。
 *
 * = set_color(BW) + 全屏提交 + flush。主帧当前内容按 BW 转换器输出。
 *
 * @return 成功返回 ESP_OK，否则返回相应错误码。
 */
esp_err_t espaperplay_gui_show_ui(void);

/**
 * @brief 切换为高清模式并整帧刷新（页面级操作，查看大图用）。
 *
 * = set_color(GRAY4) + 全屏提交 + flush。主帧当前内容按四灰阶 + 抖动
 * 转换输出（调用前请先把图片渲染进主帧缓冲）。全屏刷新较慢（~2.5s）。
 *
 * @return 成功返回 ESP_OK，否则返回相应错误码。
 */
esp_err_t espaperplay_gui_show_image(void);

/**
 * @brief 清屏为全白并全屏刷新。
 *
 * 主帧填充 RGB565 白（0xFFFF），面板走深擦除波形。
 *
 * @return 成功返回 ESP_OK，否则返回相应错误码。
 */
esp_err_t espaperplay_gui_clear(void);

#ifdef __cplusplus
}
#endif
