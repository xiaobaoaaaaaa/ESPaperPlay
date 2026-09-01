/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "espaperplay_gui.h"
#include "espaperplay_input.h"
#include "espaperplay_reader_blocks.h"
#include "espaperplay_storage.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_reader.h
 * @brief 阅读器核心框架。
 *
 * reader 组件是应用层的门面（Facade），将编排文档加载（从存储读取 TXT /
 * EPUB）、渲染（通过 GUI / EPD）与导航（来自输入事件）。
 *
 * 统一文档模型：任意格式归一为「章节 → 块序列」（见
 * espaperplay_reader_blocks.h）。TXT 是单章节文档（按行成块）；EPUB 章节表
 * 来自 OPF spine，章节按需解析驻留（同一时刻仅一个章节在内存）。渲染层
 * （screen_reader）只面向该模型，与具体格式解耦。
 */

/** 文档格式。 */
typedef enum {
    ESPAPERPLAY_READER_FMT_NONE = 0, /*!< 未打开 */
    ESPAPERPLAY_READER_FMT_TXT,      /*!< 纯文本（.txt） */
    ESPAPERPLAY_READER_FMT_EPUB,     /*!< EPUB（.epub） */
} espaperplay_reader_fmt_t;

/**
 * @brief 初始化阅读器框架。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_reader_init(void);

/**
 * @brief 从存储打开一份文档并准备阅读。
 *
 * 支持 TXT（.txt）与 EPUB（.epub），大小写不敏感。TXT 自动处理 UTF-8 BOM
 * 与换行归一化（\r\n / \r -> \n）；EPUB 解析容器 / OPF 书目（不加载正文，
 * 章节按需驻留）。路径可为绝对路径（/sdcard/...）或相对路径（books/xxx，
 * 相对于挂载点）。
 *
 * @param path 文档路径。
 *
 * @return 成功返回 ESP_OK，否则返回错误码（ESP_ERR_NOT_SUPPORTED 格式不支持、
 *         ESP_ERR_NOT_FOUND 文件不存在、ESP_ERR_NO_MEM 内存不足等）。
 */
esp_err_t espaperplay_reader_open(const char *path);

/**
 * @brief 关闭当前文档并释放缓冲。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t espaperplay_reader_close(void);

/**
 * @brief 判断当前是否有已打开的文档。
 *
 * @return 已打开返回 true。
 */
bool espaperplay_reader_is_open(void);

/**
 * @brief 获取当前文档路径（绝对路径）。
 *
 * @return 路径字符串（未打开返回 NULL，生命周期至下一次 open/close）。
 */
const char *espaperplay_reader_get_path(void);

/**
 * @brief 获取当前文档的归一化文本缓冲。
 *
 * @param[out] out_buf 文本缓冲（NUL 结尾，生命周期至 close）。
 * @param[out] out_len 文本长度（不含 NUL）。
 *
 * @return 成功返回 ESP_OK，未打开返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_reader_get_text(const char **out_buf, size_t *out_len);

/**
 * @brief 判断路径是否为阅读器支持的文档（TXT / EPUB，大小写不敏感后缀）。
 *
 * @param path 文件路径。
 * @return 支持返回 true。
 */
bool espaperplay_reader_is_supported_file(const char *path);

/**
 * @brief 判断路径是否为 TXT 文件（大小写不敏感 .txt 后缀）。
 *
 * @param path 文件路径。
 * @return 是 TXT 返回 true。
 */
bool espaperplay_reader_is_txt_file(const char *path);

/* ------------------------------------------------------------------ */
/* 统一文档模型（TXT / EPUB 共用；渲染层面向该 API）                     */
/* ------------------------------------------------------------------ */

/** 当前文档格式（未打开返回 FMT_NONE）。 */
espaperplay_reader_fmt_t espaperplay_reader_get_fmt(void);

/** 文档显示标题（TXT=文件名去后缀，EPUB=dc:title；未打开返回 NULL）。 */
const char *espaperplay_reader_get_title(void);

/** 章节数（TXT 恒为 1；未打开返回 0）。 */
int espaperplay_reader_chapter_count(void);

/**
 * @brief 加载并驻留一个章节（TXT 仅支持 0）。
 *
 * 同一时刻仅驻留一个章节；再次调用释放上一章。EPUB 章节正文按需解压解析。
 *
 * @param idx 章节号（0 基）。
 * @return ESP_OK；ESP_ERR_INVALID_STATE 未打开；ESP_ERR_NO_MEM 等。
 */
esp_err_t espaperplay_reader_load_chapter(int idx);

/** 当前驻留章节号（未驻留返回 -1）。 */
int espaperplay_reader_chapter_current(void);

/**
 * @brief 轮询式章节装载（后台路径专用；EPUB 未就绪时投递预取并返回 false）。
 *
 * TXT 恒为 true（单章节常驻）。详见 espaperplay_reader_epub.h。
 */
bool espaperplay_reader_poll_chapter(int idx);

/** 驻留章节标题（EPUB=XHTML <title>，TXT=文档标题；生命周期至下次加载/close）。 */
const char *espaperplay_reader_chapter_title(void);

/** 驻留章节文本 blob（NUL 结尾，块 off/len 指向该缓冲；未驻留返回 NULL）。 */
const char *espaperplay_reader_chapter_text(size_t *out_len);

/** 驻留章节块表（未驻留返回 NULL）。 */
const espaperplay_reader_block_t *espaperplay_reader_blocks(int *out_cnt);

/**
 * @brief 解码当前章节的一张图片（EPUB 专用；单张缓存）。
 *
 * @param img_id 块表 image id（章节内序号）。
 * @param max_w/max_h 目标框（等比缩放至内接）。
 * @param[out] out_dsc 解码结果（RGB565，模块持有，至下次调用/close）。
 * @return ESP_OK；TXT 或无图返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t espaperplay_reader_image(int img_id, int max_w, int max_h,
                                   const lv_image_dsc_t **out_dsc);

#ifdef __cplusplus
}
#endif
