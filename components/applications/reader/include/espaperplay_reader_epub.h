/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "lvgl.h"

#include "espaperplay_reader_blocks.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_reader_epub.h
 * @brief EPUB 解析后端（ZIP 容器 + OPF 章节表 + XHTML 块化 + 图片解码）。
 *
 * 面向 ESP32-S3 + PSRAM 的低内存设计：
 *   - 解析与渲染解耦：章节解析（解压 + CSS + XHTML 块化）在内部 RAM 栈的
 *     预取 worker 任务执行（优先级低于 LVGL），产出数据包放入就绪槽；
 *     LVGL 线程只做采纳与 FreeType 分页，翻章零解析阻塞；
 *   - SD 解析缓存：数据包二进制落盘（/sdcard/system/cache/reader/，键 =
 *     路径哈希 ^ mtime ^ size，书变更自动失效），二次打开章节装载为一次
 *     小文件读取；缓存写全部在 worker（SD 写不进 LVGL 线程）；
 *   - ZIP 按需解压：只解析中央目录（常驻小表），条目数据用 zlib raw inflate
 *     流式解压到 PSRAM，用后即释放，不整书载入（zip 访问带递归互斥，
 *     worker 解析与 LVGL 图片解码可并发）；
 *   - 章节以目录为准（EPUB2 toc.ncx navMap / EPUB3 nav），而非 spine 文档
 *     数——目录外的独立插图文档并入相邻章节的阅读流，内容不丢；无目录时
 *     退化为 spine 文档即章；
 *   - 章节按需驻留：同一时刻只保留一个章节的文本 blob + 块表（章 = 多个
 *     spine 文档顺序拼接）；
 *   - CSS 版式子集：章节样式表按 zip 条目缓存解析——类规则对齐 / 粗斜体 /
 *     text-indent 覆盖与元素规则 p{text-indent}（正文首行缩进），块级样式
 *     随标签开合继承给内部段落；字体族 / 颜色 / 阴影不还原；
 *   - 图片单页渲染：JPEG 走 TJpgD 逐 MCU 解码、PNG 走 zlib 流式逐行解码
 *     （unfilter + 行内抽样，支持 1-16 位深全颜色类型与调色板 / tRNS，
 *     峰值内存 ≈ 2 行扫描线），均在解码中抽样缩放（目标缓冲 + KB 级工作区，
 *     不做整图 RGBA 解码）；同一时刻只缓存一张图。
 */

/** 单个 EPUB 条目（图片/字体）解压后的最大体积（超出按解码失败处理）。 */
#define ESPAPERPLAY_EPUB_MAX_ENTRY_BYTES (4 * 1024 * 1024)

/** 判断路径是否为 EPUB 文件（大小写不敏感 .epub 后缀）。 */
bool espaperplay_reader_is_epub(const char *path);

/** 打开 EPUB（解析容器 / OPF / 书目 spine；不加载章节正文）。 */
esp_err_t espaperplay_reader_epub_open(const char *abs_path);

/** 关闭并释放全部资源（含驻留章节与图片缓存）。 */
void espaperplay_reader_epub_close(void);

/** 是否已打开。 */
bool espaperplay_reader_epub_is_open(void);

/** 书名（OPF dc:title；无则空串）。生命周期至 close。 */
const char *espaperplay_reader_epub_title(void);

/** 章节数（spine 中的 XHTML 文档数）。 */
int espaperplay_reader_epub_chapter_count(void);

/**
 * @brief 加载并驻留一个章节（XHTML 解析为块模型）。
 *
 * 同一时刻仅驻留一个章节；再次调用会释放上一章。解析为单遍状态机：
 * 标题层级 / 加粗 / 斜体 / 引用 / 对齐 / 首行缩进 → 块 flags（块级样式
 * 随标签开合栈式继承），<img> → 图片块，ruby 注音（rt/rp）与
 * style/script/head 内容丢弃，实体解码、空白折叠；首个 <link> 样式表
 * 按 zip 条目缓存做 CSS 子集解析。
 *
 * @param idx 章节（spine 序，0 基）。
 * @return ESP_OK；ESP_ERR_NOT_FOUND / ESP_ERR_NO_MEM / ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_reader_epub_load_chapter(int idx);

/* ---------------- 分页数据 SD 缓存（页边界表） ---------------- */

/**
 * @brief 读取章节分页缓存（LVGL 线程可调，只读）。
 *
 * @param chapter  章节号。
 * @param font_key 版式指纹（字号档位 + 内容区宽高；版式变化自动失效）。
 * @param blocks   输出数组（每页起始块号，容量 max_cnt）。
 * @param lines    输出数组（每页起始块内行号，容量 max_cnt）。
 * @param max_cnt  数组容量。
 * @return 缓存的页数（>0 命中）；0=无缓存；<0=读取失败（文件已删除待重建）
 *         或 -所需容量（缓存有效但 max_cnt 不足，调用方扩容后重读，文件保留）。
 */
int espaperplay_reader_epub_pagen_load(int chapter, uint32_t font_key, uint32_t *blocks,
                                       uint16_t *lines, int max_cnt);

/**
 * @brief 异步保存章节分页缓存（LVGL 线程调用；数组复制后由 worker 落盘）。
 *
 * @param chapter  章节号。
 * @param font_key 版式指纹。
 * @param cnt      页数（页 0 恒为 (0,0)，调用方仍需完整给出 cnt 项）。
 * @param blocks   页起始块号数组。
 * @param lines    页起始块内行号数组。
 */
void espaperplay_reader_epub_pagen_save_async(int chapter, uint32_t font_key, int cnt,
                                              const uint32_t *blocks, const uint16_t *lines);

/** 当前驻留章节号（未驻留返回 -1）。 */
int espaperplay_reader_epub_chapter_current(void);

/**
 * @brief 轮询式章节装载（后台计数等非交互路径专用，绝不内联解析）。
 *
 * 就绪槽（worker 预取）命中 → 采纳为驻留章并返回 true；未命中 → 向 worker
 * 投递预取请求并返回 false，调用方下一轮再试。与 load_chapter 的区别：
 * 不做缓存读 / 内联解析兜底，保证调用线程不被百毫秒级解析阻塞。
 */
bool espaperplay_reader_epub_poll_chapter(int idx);

/** 驻留章节标题（XHTML <title>；生命周期至下一次加载/close）。 */
const char *espaperplay_reader_epub_chapter_title(void);

/** 驻留章节文本 blob（NUL 结尾，块 off/len 指向该缓冲）。 */
const char *espaperplay_reader_epub_chapter_text(size_t *out_len);

/** 驻留章节块表。 */
const espaperplay_reader_block_t *espaperplay_reader_epub_blocks(int *out_cnt);

/**
 * @brief 解码当前章节的一张图片（单张缓存，重复调用同 id 直接命中）。
 *
 * JPEG：TJpgD 逐 MCU 流式解码，输出回调内最近邻抽样缩放进 max_w×max_h 预算框；
 * PNG：zlib 流式逐行 unfilter + 行内抽样（整图 RGBA 解码在 LVGL 分配器下
 * 不可行，大图会 lodepng alloc 失败）。
 *
 * @param img_id 块表中的 image id（章节内序号）。
 * @param max_w 解码预算框宽（像素；渲染层可再等比缩放至实际显示框）。
 * @param max_h 解码预算框高（像素）。
 * @param[out] out_dsc 解码结果（RGB565，缓冲由模块持有，至下次调用/close）。
 * @return ESP_OK；其他错误码见日志。
 */
esp_err_t espaperplay_reader_epub_image(int img_id, int max_w, int max_h,
                                        const lv_image_dsc_t **out_dsc);

#ifdef __cplusplus
}
#endif
