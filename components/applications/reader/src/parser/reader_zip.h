/* SPDX-License-Identifier: MIT */
#pragma once
#include "reader_parse.h"
typedef struct {
    uint32_t lfh_off, csize, usize, crc, name_off;
    uint16_t method, flags, name_len;
} epub_zip_ent_t;
typedef struct {
    FILE *fp;
    epub_zip_ent_t *ents;
    int ent_cnt;
    char *names;
    size_t names_len;
    uint32_t file_size, cd_offset;
    int *index;
    size_t index_size;
} epub_zip_t;
esp_err_t epub_zip_parse(epub_zip_t *z);
void rp_zip_close(epub_zip_t *z);
int epub_zip_find(const epub_zip_t *z, const char *name);
bool epub_zip_ent_name(const epub_zip_t *z, int idx, char *buf, size_t cap);
esp_err_t epub_zip_extract_unlocked(epub_zip_t *z, int idx, char **out, size_t *len);
esp_err_t rp_zip_xml(epub_zip_t *z, int idx, char **out, size_t *len);
