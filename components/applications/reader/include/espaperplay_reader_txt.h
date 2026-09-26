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

/* TXT format detection. Decoding/indexing are private reader_text contexts. */

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
