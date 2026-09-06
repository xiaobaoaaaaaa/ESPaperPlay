/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_reader_pagen.h
 * @brief 分页数据（页边界表）SD 缓存，TXT / EPUB 通用。
 *
 * 缓存文件位于 /sdcard/system/cache/reader/<token>.chNNN.f<font_key>.pag；
 * token 为书指纹（路径哈希 ^ mtime ^ size，由调用方提供），书变更或版式
 * 变化（font_key）自动失效。读取为同步只读（LVGL 线程可调）；写入复制
 * 参数后由本模块 worker 异步落盘（SD 写不进 LVGL 线程）。
 */

/**
 * @brief 读取章节分页缓存（同步只读）。
 *
 * @param token    书指纹。
 * @param chapter  章节号（TXT 恒为 0）。
 * @param font_key 版式指纹（字号档位 + 内容区宽高）。
 * @param blocks   输出数组（每页起始块号，容量 max_cnt）。
 * @param lines    输出数组（每页起始块内行号，容量 max_cnt）。
 * @param max_cnt  数组容量。
 * @return 缓存页数（>0 命中）；0=无缓存；<0=读取失败（损坏文件已删除，
 *         或 -所需容量：缓存有效但 max_cnt 不足，调用方扩容后重读）。
 */
int espaperplay_pagen_load(uint32_t token, int chapter, uint32_t font_key, uint32_t *blocks,
                           uint16_t *lines, int max_cnt);

/**
 * @brief 异步保存章节分页缓存（数组复制后由 worker 落盘，原子替换写）。
 *
 * @param token    书指纹。
 * @param chapter  章节号。
 * @param font_key 版式指纹。
 * @param cnt      页数（含页 0）。
 * @param blocks   页起始块号数组。
 * @param lines    页起始块内行号数组。
 */
void espaperplay_pagen_save_async(uint32_t token, int chapter, uint32_t font_key, int cnt,
                                  const uint32_t *blocks, const uint16_t *lines);

#ifdef __cplusplus
}
#endif
