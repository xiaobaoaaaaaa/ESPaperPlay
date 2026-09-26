/* SPDX-License-Identifier: MIT */
#pragma once
#include "reader_zip.h"
#define RP_CSS_RULES 256
#define RP_STYLE_BOLD 1
#define RP_STYLE_ITALIC 2
#define RP_STYLE_ALIGN 4
#define RP_STYLE_INDENT 8
#define RP_STYLE_HIDDEN 16
#define RP_STYLE_SPACE 32
typedef struct {
    uint16_t flags;
    bool indent, hidden, pre;
} rp_style_t;
typedef struct {
    char selector[160];
    rp_style_t style;
    unsigned mask, important;
} rp_css_rule_t;
typedef struct {
    rp_css_rule_t rules[RP_CSS_RULES];
    int count;
} rp_css_t;
typedef struct {
    char name[48], id[128], classes[192];
    rp_style_t style;
    int list_number;
    bool block, skip;
} rp_element_t;
void rp_css_parse(rp_css_t *css, const char *source, size_t len);
void rp_css_resolve(const rp_css_t *css, rp_element_t *stack, int depth, const char *inline_style);
esp_err_t rp_css_document(epub_zip_t *zip, const char *path, const char *html, size_t len,
                          rp_css_t *css);
