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
#include "espaperplay_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_reader.h
 * @brief 阅读器核心框架。
 *
 * reader 组件是应用层的门面（Facade），将编排文档加载（从存储读取 TXT /
 * EPUB / PDF）、渲染（通过 GUI / EPD）与导航（来自输入事件）。当前已实现
 * TXT 解析与分页基础，后续扩展 EPUB / PDF。
 */

/**
 * @brief 初始化阅读器框架。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_reader_init(void);

/**
 * @brief 从存储打开一份文档并准备阅读。
 *
 * 仅支持 TXT（大小写不敏感 .txt 后缀），自动处理 UTF-8 BOM 与换行归一化
 * （\r\n / \r -> \n）。路径可为绝对路径（/sdcard/...）或相对路径
 * （books/example.txt，相对于挂载点）。
 *
 * @param path 文档路径。
 *
 * @return 成功返回 ESP_OK，否则返回错误码（ESP_ERR_NOT_SUPPORTED 非 TXT、
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
 * @brief 判断路径是否为 TXT 文件（大小写不敏感 .txt 后缀）。
 *
 * @param path 文件路径。
 * @return 是 TXT 返回 true。
 */
bool espaperplay_reader_is_txt_file(const char *path);

#ifdef __cplusplus
}
#endif
