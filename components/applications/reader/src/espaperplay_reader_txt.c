/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_reader_txt.h"

#include <ctype.h>
#include <string.h>

void espaperplay_reader_txt_normalize(char *buf, size_t *len) {
    if (buf == NULL || len == NULL || *len == 0) {
        return;
    }
    size_t n = *len;
    /* 去除 UTF-8 BOM */
    if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, n - 3);
        n -= 3;
    }
    /* 换行归一化：\r\n -> \n，\r -> \n，原地处理 */
    size_t r = 0;
    size_t w = 0;
    while (r < n) {
        if (buf[r] == '\r') {
            buf[w++] = '\n';
            r++;
            if (r < n && buf[r] == '\n') {
                r++; /* 跳过 \r\n 中的 \n */
            }
        } else {
            buf[w++] = buf[r++];
        }
    }
    buf[w] = '\0';
    *len = w;
}

bool espaperplay_reader_is_txt(const char *path) {
    if (path == NULL) {
        return false;
    }
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return false;
    }
    /* 大小写不敏感比较 .txt */
    if (strcasecmp(dot, ".txt") == 0) {
        return true;
    }
    return false;
}
