/* SPDX-License-Identifier: MIT */
#pragma once
#include "reader_css.h"
typedef struct {
    char *text;
    size_t text_len, text_cap, paragraph;
    espaperplay_reader_block_t *blocks;
    int block_count, block_cap;
    int *images;
    int image_count, image_cap;
    uint16_t paragraph_flags;
    bool pending_space;
    char title[128];
} rp_content_t;
void rp_content_free(rp_content_t *content);
esp_err_t rp_content_document(epub_zip_t *zip, const char *path, const char *html, size_t len,
                              uint32_t start, uint32_t end, rp_content_t *content);
