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
 * ZIP、XML/URI、OPF/导航、CSS、XHTML、图片解码分别在 src/parser 下实现。
 * 显式资源上限、支持范围与测试方法见 docs/reader-engine.md。
 *
 * 章节由 EPUB3 nav（优先）/ EPUB2 NCX 与 spine 阅读顺序共同构建，保留锚点。
 * 当前章节和一个后台预取结果分别拥有自己的文本、块表和资源编号。
 * 交互装载优先采纳预取 / SD 缓存，未命中时同步解析；后台轮询走 worker。
 * worker 使用 PSRAM 栈，写入带引擎版本的缓存，换书通过锁和会话代数隔离。
 * JPEG / PNG 按目标尺寸逐 MCU / 逐行抽样，只缓存一张解码图片。

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

/** 章节数（目录项数；无目录时 = spine 文档数）。 */
int espaperplay_reader_epub_chapter_count(void);

/**
 * @brief 获取章节的目录标题（打开时已从目录解析，无需装载章节）。
 *
 * @param idx  章节号（0 基）。
 * @param buf  输出缓冲（NUL 结尾）。
 * @param len  缓冲长度。
 * @return 有非空标题返回 true；无目录 / 空标题 / 越界返回 false。
 */
bool espaperplay_reader_epub_toc_title(int idx, char *buf, size_t len);

/**
 * @brief 加载并驻留一个章节（XHTML 解析为块模型）。
 *
 * 成功后转移结果包所有权并释放上一章；失败保留原驻留结果。
 * 文档样式按继承和层叠合成；结构和 Ruby 注音保留为可读块流。
 *
 * @param idx 目录章节号（0 基，可对应同一 spine 文档内的不同锚点）。
 * @return ESP_OK；ESP_ERR_NOT_FOUND / ESP_ERR_NO_MEM / ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_reader_epub_load_chapter(int idx);

/** 书指纹（路径哈希 ^ mtime ^ size ^ 引擎版本）：分页缓存等外部数据的键。 */
uint32_t espaperplay_reader_epub_token(void);

/** 当前驻留章节号（未驻留返回 -1）。 */
int espaperplay_reader_epub_chapter_current(void);

/**
 * @brief 轮询式章节装载（后台计数等非交互路径专用，worker 可用时不内联解析）。
 *
 * 就绪槽（worker 预取）命中 → 采纳为驻留章并返回 true；未命中 → 向 worker
 * 投递预取请求并返回 false，调用方下一轮再试。与 load_chapter 的区别：
 * worker 创建失败时同步降级。坏章以空驻留结果完成，避免无限轮询；
 * 再次 load_chapter 返回其错误状态。
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
 * @param[out] out_dsc 解码结果（RGB565，模块持有，至下次图片调用/close；切换章节不失效）。
 * @return ESP_OK；其他错误码见日志。
 */
esp_err_t espaperplay_reader_epub_image(int img_id, int max_w, int max_h,
                                        const lv_image_dsc_t **out_dsc);

/**
 * @brief 轻量探测一份 EPUB 文件的封面（书架缩略图等场景）。
 *
 * 独立轻量流程：自持 zip 上下文解析中央目录 + container.xml + OPF，定位封面
 * 条目后解压解码，全程不触碰已打开书籍的全局状态（不建预取 worker / 目录表，
 * 不占图片单槽缓存），可与正常阅读并发。封面定位优先级：EPUB3 manifest
 * properties~="cover-image" → EPUB2 &lt;meta name="cover"&gt; 指向的 manifest id →
 * 基名 cover.jpg/jpeg/png → spine 首文档内首个图片。
 *
 * @param abs_path EPUB 绝对路径。
 * @param max_w 解码预算框宽（像素；解码后等比降采样保证不超出）。
 * @param max_h 解码预算框高（像素）。
 * @param[out] out_dsc 解码结果（RGB565）。
 * @param[out] out_buf 像素缓冲（PSRAM 堆分配，所有权归调用方，用完 heap_caps_free）。
 * @return ESP_OK；ESP_ERR_NOT_FOUND 无封面 / 文件不存在；其他错误码见日志。
 */
esp_err_t espaperplay_reader_epub_probe_cover(const char *abs_path, int max_w, int max_h,
                                              lv_image_dsc_t *out_dsc, uint8_t **out_buf);

/* 段内样式与章节正文同生命周期；TXT 返回空表。 */
const espaperplay_reader_span_t *espaperplay_reader_epub_spans(int *count);

#ifdef __cplusplus
}
#endif
