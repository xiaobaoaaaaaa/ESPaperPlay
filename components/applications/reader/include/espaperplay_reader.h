/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

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
 * EPUB / PDF）、渲染（通过 GUI / EPD）与导航（来自输入事件）。当前阶段仅
 * 存在框架骨架，尚未实现任何文档格式。
 */

/**
 * @brief 初始化阅读器框架。
 *
 * @note 当前仅为骨架。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_reader_init(void);

/**
 * @brief 从存储打开一份文档并准备阅读。
 *
 * @param path 文档在文件系统中的路径（相对于存储挂载点），
 *             例如 "books/example.epub"。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_reader_open(const char *path);

#ifdef __cplusplus
}
#endif
