/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_reader_txt.h
 * @brief TXT 解析辅助（UTF-8 BOM / 换行归一化）。
 */

/**
 * @brief 归一化 TXT 文本（BOM 去除 + 换行统一为 \n）。
 *
 * 原地处理：\r\n -> \n，\r -> \n，UTF-8 BOM（EF BB BF）去除。
 *
 * @param[in,out] buf 文本缓冲（可修改）。
 * @param[in,out] len 缓冲长度（输入为原始长度，输出为归一化后长度）。
 */
void espaperplay_reader_txt_normalize(char *buf, size_t *len);

/**
 * @brief 判断路径是否为 TXT 文件（大小写不敏感 .txt 后缀）。
 *
 * @param path 文件路径。
 * @return 是 TXT 返回 true。
 */
bool espaperplay_reader_is_txt(const char *path);

#ifdef __cplusplus
}
#endif
