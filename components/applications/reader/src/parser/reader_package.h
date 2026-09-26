/* SPDX-License-Identifier: MIT */
#pragma once
#include "reader_zip.h"
#define RP_MAX_ITEMS 4096
#define RP_MAX_CHAPTERS 4096
typedef struct {
    char *zip_path, *id, *fallback;
    uint8_t kind; /* 1 XHTML/SVG, 2 image, 3 font, 4 CSS, 5 NCX */
    bool nav, cover;
} rp_item_t;
typedef struct {
    int doc;
    uint32_t offset;
    char title[128];
} rp_chapter_t;
typedef struct {
    rp_item_t *items;
    int item_cnt, *spine, spine_cnt;
    rp_chapter_t *chapters;
    int chapter_count;
    char title[128], opf_path[RP_PATH];
    int cover_entry;
} rp_package_t;
esp_err_t rp_package_open(epub_zip_t *zip, rp_package_t *book);
void rp_package_free(rp_package_t *book);

esp_err_t rp_package_metadata(epub_zip_t *zip, rp_package_t *book);
