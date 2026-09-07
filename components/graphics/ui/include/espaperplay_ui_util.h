/*
 * ESPaperPlay - UI 屏幕通用工具
 *
 * 各屏幕页面（screen_*.c）共用的几何/字体/标签/路径小工具。
 * 目的：把原先每屏各写一份的同构 static 函数收敛为单一实现，
 * 墨水屏相关的特调（EPD 局刷去重、FreeType 字体缓存键）也在此收口。
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 屏幕尺寸 / 屏高缩放                                                   */
/* ------------------------------------------------------------------ */

/** 默认显示器的分辨率（lv_display 包装）。 */
void espaperplay_ui_screen_size(int32_t *out_w, int32_t *out_h);

/**
 * 设置屏高缩放因子：scale = 屏高 / ref_h。
 * 各页面 enter 时以自己的基准逻辑高度调用（页面栈保证同时只有一个页面在前台，
 * 因此因子为全局单份）。
 */
void espaperplay_ui_scale_init(int32_t ref_h);

/** 基准值按屏高缩放（取整）。须先经 espaperplay_ui_scale_init 设置。 */
int espaperplay_ui_scaled(int v);

/* ------------------------------------------------------------------ */
/* 字体 / 标签                                                           */
/* ------------------------------------------------------------------ */

/**
 * 加载当前选用字体（SD 优先，缺则回退 Flash 子集；名字为空同样回退）。
 * FreeType 缓存按 (名,号,式) 取项，各屏共用同一缓存键。
 */
lv_font_t *espaperplay_ui_font(int size_px);

/** 通用标签：黑字 + FreeType 字体 + 对齐 + 100% 宽 + 禁滚动（防误滑页面）。 */
lv_obj_t *espaperplay_ui_label_create(lv_obj_t *parent, const char *text, int font_px,
                                      lv_text_align_t align);

/**
 * 仅当文本变化时才写入标签（EPD 上避免无谓局刷）。
 * @return true=实际更新了标签；false=文本未变（或参数无效）。
 */
bool espaperplay_ui_label_set_text_dedup(lv_obj_t *label, const char *text);

/* ------------------------------------------------------------------ */
/* 命中检测 / 屏幕坐标                                                   */
/* ------------------------------------------------------------------ */

/** 点 p 是否落在矩形 (x,y,w,h) 内（左闭右开）。 */
bool espaperplay_ui_point_in(const lv_point_t *p, int x, int y, int w, int h);

/** 对象相对屏幕的绝对 x/y（逐级累加父偏移；顶层容器返回 0 基准）。 */
int espaperplay_ui_obj_screen_x(const lv_obj_t *obj);
int espaperplay_ui_obj_screen_y(const lv_obj_t *obj);

/* ------------------------------------------------------------------ */
/* 文本 / 路径                                                           */
/* ------------------------------------------------------------------ */

/** UTF-8 边界安全截断：超长回退到字符边界并追加省略号 "…"。 */
void espaperplay_ui_utf8_truncate(const char *src, char *dst, size_t n);

/** 路径拼接 a + "/" + b，检测缓冲溢出（不写半截路径）。@return false=缓冲不足 */
bool espaperplay_ui_path_join(char *dst, size_t n, const char *a, const char *b);

/** 取路径最后一段（文件名；无 '/' 或以 '/' 结尾时返回原串）。 */
const char *espaperplay_ui_path_basename(const char *path);

/* ------------------------------------------------------------------ */
/* 页点指示器（分页）                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 创建页点指示器：count 个 10px 圆点水平居中于 scr_w。
 * @param dots    输出对象数组（调用方持有，用于后续着色/删除）。
 * @param count   页数。
 * @param scr_w   屏宽（居中基准）。
 * @param y       圆点顶部 y（常用 scr_h - 18，或底部栏上方）。
 * @param spacing 圆点中心间距（像素；0 = 默认 24）。
 */
void espaperplay_ui_pager_dots_create(lv_obj_t **dots, int count, int32_t scr_w, int y,
                                      int spacing);

/** @brief 页点着色：current 页黑（实心），其余白（黑边空心）。 */
void espaperplay_ui_pager_dots_set(lv_obj_t **dots, int count, int current);

/* ------------------------------------------------------------------ */
/* WiFi 信号分档图标                                                     */
/* ------------------------------------------------------------------ */

/**
 * RSSI -> 4 档信号图标（>=-60 满格，>=-70 三格，>=-80 两格，其余一格）。
 * 状态栏与 wifi 列表共用同一分档（此前两处阈值不一致）。
 */
const lv_image_dsc_t *espaperplay_ui_wifi_rssi_icon(int rssi);

#ifdef __cplusplus
}
#endif
