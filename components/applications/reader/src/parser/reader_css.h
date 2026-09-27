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
#define RP_STYLE_SIZE 64
#define RP_STYLE_LINE 128
#define RP_STYLE_BEFORE 256
#define RP_STYLE_AFTER 512
#define RP_STYLE_DECORATION 1024
/* CSS value before computation: hundredths, unit 0=em, 1=root em, 2=px. */
typedef struct {
    int16_t value;
    uint8_t unit;
} rp_length_t;
typedef struct {
    uint16_t flags;
    bool hidden, pre;
    uint16_t scale, line_height, layout;
    int16_t indent, before, after;
    rp_length_t size_value, indent_value, line_value, before_value, after_value;
    unsigned inherit, initial;

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
