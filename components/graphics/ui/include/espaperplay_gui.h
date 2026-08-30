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
 *   - 单一 RGB565 主帧缓冲（分辨率来自 espaperplay_display，默认 800x480，
 *     渲染目标（后续 LVGL 配 LV_COLOR_DEPTH=16 直接画入）；
 *   - flush 时按当前模式把 RGB 快照转换为 e-paper 帧格式（帧内容冻结）：
 *       · 交互模式（BW）：RGB -> 1bpp，脏区转换 + 局部刷新（~370ms），
 *         默认阈值转换（LVGL 抗锯齿字体的中间灰直接切边，文字锐利）；
 *         Bayer 4x4 有序抖动为可选（纯黑/纯白位精确，适合无抗锯齿的
 *         位图内容）；
 *       · 高清模式（GRAY4）：RGB -> 2bpp 四灰阶，默认 Floyd–Steinberg
 *         误差扩散抖动（质量最好；Bayer 4x4 为快速选项），整帧转换 +
 *         全屏刷新（~2.5s，仅全屏）；
 *   - 刷新异步化：flush() 只快照并排队，真实刷新由内部 worker 任务执行
 *     （阻塞只发生在 worker），渲染/调用线程永不被屏幕刷新阻塞；双槽位
 *     允许渲染器连续提交，worker 顺序消费并天然节流。需要同步点时用
 *     espaperplay_gui_wait_idle()；
 *   - 脏区为单包围盒（8 对齐，帧内合并）；全刷策略：无论局刷区域大小，
 *     每次 BW 局部刷新都累计连续局刷次数，连续达到
 *     ESPAPERPLAY_GUI_FULL_FORCE_AFTER 次后强制一次全像素翻转全刷，
 *     清除局刷累积的残影（GRAY4/CLEAR 恒全屏）；
 *   - 模式切换是页面级操作（espaperplay_gui_show_bw / show_gray4），
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
    ESPAPERPLAY_GUI_CONVERTER_THRESHOLD = 0, /*!< 阈值（L>=128 白），无抖动，速度快；默认。
                                              文字锐利（抗锯齿灰边直接切），适合 UI */
    ESPAPERPLAY_GUI_CONVERTER_BAYER,         /*!< Bayer 4x4 有序抖动：中间灰呈点阵，纯黑/纯白
                                              位精确（0/255 直通）。抗锯齿字体会被点阵化（边缘
                                              发糊），适合无灰度的位图内容 */
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
 * 使能后 espaperplay_gui_init() 会创建一个后台任务，依次验证：清白、
 * 纯黑白 UI 位精确性（Bayer 无伪影）、灰度渐变（Bayer）、阈值对照、
 * 四种转换路径性能计时、gray4 高清（FS 与 Bayer 对比）、模式切换无残留，
 * 最后恢复黑白并清白。每步 flush 后 wait_idle 确认画面已更新（worker 日志
 * 含各次刷新的真实耗时）。
 */
#ifndef ESPAPERPLAY_GUI_ENABLE_SELFTEST
#define ESPAPERPLAY_GUI_ENABLE_SELFTEST 0
#endif

/**
 * @brief 连续局刷达到该次数后，强制执行一次全像素翻转全刷清残影。
 *
 * 无论局刷区域大小，每次 BW 局部刷新都会累计；连续达到该次数后下一次
 * 刷新强制全刷（画面闪黑一下）。该宏仅作为上电初始默认值；运行期可用
 * espaperplay_gui_set_full_force_after() 调整（Web 可配置并经 NVS 持久化），
 * 置 0 表示禁用周期性全刷清残影（永远只用局部刷新）。
 */
#ifndef ESPAPERPLAY_GUI_FULL_FORCE_AFTER
#define ESPAPERPLAY_GUI_FULL_FORCE_AFTER 20
#endif

/** 连续局刷计数阈值上限（防止不合理配置）。 */
#define ESPAPERPLAY_GUI_FULL_FORCE_AFTER_MAX 255u

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
 * @brief 切换刷新模式（BW 交互 / GRAY4 高清）。
 *
 * 只改变 flush 的转换路径与驱动模式，不触发刷新、不清空主帧。切换后请
 * 重新提交全屏区域并 flush（或直接调用 show_bw / show_gray4）。
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
 * 默认 THRESHOLD（UI 文字锐利；图像走 GRAY4 模式，不受影响）。
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
 * @brief 设置"连续局刷 N 次后强制全刷清残影"的计数阈值（0 = 禁用）。
 *
 * 立即生效：下一次 flush 快照即按新阈值决策。置 0 后永远不再强制全刷
 * （只做局部刷新）；运行期可经 Web 修改并经 NVS 持久化。
 *
 * @param count 阈值（0..255；0 = 禁用）。
 *
 * @return 成功返回 ESP_OK；未初始化返回 ESP_ERR_INVALID_STATE；越界返回
 *         ESP_ERR_INVALID_ARG。
 */
esp_err_t espaperplay_gui_set_full_force_after(uint32_t count);

/**
 * @brief 获取当前"连续局刷后强制全刷"的计数阈值（0 = 禁用）。
 *
 * @return 当前阈值。
 */
uint32_t espaperplay_gui_get_full_force_after(void);

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
 * 包围盒（一帧内可多次调用，flush() 时合并为一次刷新）。
 *
 * @return 成功返回 ESP_OK；未初始化返回 ESP_ERR_INVALID_STATE；空区域
 *         忽略并返回 ESP_OK。
 */
esp_err_t espaperplay_gui_submit_area(uint16_t x, uint16_t y, uint16_t width, uint16_t height);

/**
 * @brief 排队一次合并后的转换 + 刷新（异步，不阻塞调用方）。
 *
 * 在调用线程内把脏区从 RGB565 主帧快照转换进暂存（帧内容冻结），然后交给
 * 内部 worker 任务执行真实刷新（局部/全屏/灰阶按模式与面积选择）。刷新
 * 期间 LVGL 等渲染任务不会被阻塞；若已有两帧排队，新脏区合并进待刷帧。
 * 需要等待屏幕真正更新完成时使用 espaperplay_gui_wait_idle()。
 *
 * @return 成功返回 ESP_OK（已排队）；无脏区返回 ESP_OK（空操作）；
 *         未初始化返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_gui_flush(void);

/**
 * @brief 等待内部刷新 worker 排空（可选同步点）。
 *
 * 阻塞直到所有已排队刷新完成，或超时。用于自检、清屏后、"需要确认画面
 * 已更新"的应用路径；常规渲染流程无需调用。
 *
 * @param timeout_ms 超时（毫秒）。
 *
 * @return ESP_OK；超时返回 ESP_ERR_TIMEOUT；未初始化返回
 *         ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_gui_wait_idle(uint32_t timeout_ms);

/**
 * @brief 切换到 BW 模式并整帧刷新（页面级操作，GRAY4 -> BW 转换）。
 *
 * = set_color(BW) + 全屏提交 + flush。主帧当前内容按 BW 转换器输出；
 * 驱动自动清除灰阶残留（反相翻转）。从 GRAY4 切回交互界面时调用。
 *
 * @return 成功返回 ESP_OK，否则返回相应错误码。
 */
esp_err_t espaperplay_gui_show_bw(void);

/**
 * @brief 切换到 GRAY4 模式并整帧刷新（页面级操作，BW -> GRAY4 转换）。
 *
 * = set_color(GRAY4) + 全屏提交 + flush。主帧当前内容按四灰阶 + 抖动
 * 转换输出（调用前请先把图片渲染进主帧缓冲）。全屏刷新较慢（~2.5s）。
 *
 * @return 成功返回 ESP_OK，否则返回相应错误码。
 */
esp_err_t espaperplay_gui_show_gray4(void);

/**
 * @brief 强制执行一次全屏刷新（清残影 / 手动全刷）。
 *
 * 对当前 RGB565 主帧整帧快照转换后排队一次全屏深刷新，不改变刷新模式：
 *   - BW 模式：FULL_FORCE —— 全像素翻转走深擦除波形，彻底清除局刷累积的
 *     残影（画面会闪黑一下，约 1.7s）；
 *   - GRAY4 模式：整帧四灰阶全屏刷新（~2.5s）。
 * 丢弃尚未执行的排队帧（全刷建立新基线），异步执行（worker 消费，本函数
 * 不阻塞，仅快照排队）；如需确认完成请调用 espaperplay_gui_wait_idle()。
 *
 * 供用户主动触发（如 BOOT 键长按的全局默认动作）与测试使用。
 *
 * @return 成功返回 ESP_OK（已排队）；未初始化返回 ESP_ERR_INVALID_STATE；
 *         槽位全忙返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_gui_full_refresh(void);

/**
 * @brief 清屏为全白并全屏刷新（异步排队）。
 *
 * 主帧填充 RGB565 白（0xFFFF），丢弃尚未刷新的排队帧，面板走深擦除波形。
 * 需要确认清屏完成时调用 espaperplay_gui_wait_idle()。
 *
 * @return 成功返回 ESP_OK，否则返回相应错误码。
 */
esp_err_t espaperplay_gui_clear(void);

#ifdef __cplusplus
}
#endif
