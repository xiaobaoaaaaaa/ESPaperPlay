/* SPDX-License-Identifier: MIT */
#pragma once
#include "reader_parse.h"
/* A book owns its file, compact raw byte offsets and one decoded segment. */
typedef enum { RP_UTF8, RP_UTF16LE, RP_UTF16BE, RP_UTF32LE, RP_UTF32BE, RP_GB18030 } rp_encoding_t;
typedef struct {
    FILE *fp;
    char *io;
    uint32_t pos, size, *offsets;
    int count, current;
    rp_encoding_t encoding;
    bool invalid;
    esp_err_t status;
    char *text;
    size_t text_len;
    espaperplay_reader_block_t *blocks;
    int block_count;
} rp_text_t;
esp_err_t rp_text_open(rp_text_t *book, const char *path);
void rp_text_close(rp_text_t *book);
esp_err_t rp_text_load(rp_text_t *book, int chapter);
