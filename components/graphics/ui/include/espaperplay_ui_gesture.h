/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - 触摸手势阈值（单一来源，仅头文件宏）
 *
 * 各屏 on_touch 共用的物理像素阈值（不随屏高缩放）。此前 7 个屏幕各定义
 * 一份同值宏，存在漂移风险（RSSI 分档已实际漂移过一次）；现统一为本文件，
 * 各屏以本地别名引用，调一处全局生效。
 *
 * 注：手势状态机本体（按下/释放分类、长按锁存、模态点击抑制）仍留在各屏
 * ——各页语义差异大（weather 的滚动区/预警条分区、reader 的点按翻页区），
 * 且改动需实机触摸回归验证，暂不强行抽象。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 滑动 / 点击（物理像素） ---- */
#define UI_GESTURE_EDGE_PX 24         /*!< 边缘滑动触发宽度 */
#define UI_GESTURE_EDGE_SWIPE_PX 70   /*!< 边缘向内滑动位移阈值 */
#define UI_GESTURE_SWIPE_PX 90        /*!< 分页切换位移阈值 */
#define UI_GESTURE_CLICK_MAX_PX 15    /*!< 点击允许的最大位移（防抖） */
#define UI_GESTURE_SWIPE_MIN_RATIO 1.2f /*!< 横向位移 / 纵向位移 最小比例 */

/* ---- 长按 / 模态 ---- */
#define UI_GESTURE_LONG_PRESS_MS 600      /*!< 长按判定时长 */
#define UI_GESTURE_MODAL_GUARD_MS 300     /*!< 按住期间弹出模态的点击抑制下限 */
#define UI_GESTURE_MODAL_RELEASE_GRACE_MS 150 /*!< 观察到物理释放后的额外抑制宽限 */

#ifdef __cplusplus
}
#endif
