/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "espaperplay_reader_blocks.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Parser allocations live in PSRAM on device; the host tests use the same code. */
#define RP_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define RP_PATH 600
#define RP_MAX_TEXT (2u * 1024 * 1024)
#define RP_MAX_BLOCKS 20000
#define RP_MAX_IMAGES 256
#define RP_MAX_DEPTH 64
static inline void *rp_alloc(size_t n) { return heap_caps_malloc(n, RP_CAPS); }
static inline void *rp_realloc(void *p, size_t n) { return heap_caps_realloc(p, n, RP_CAPS); }
static inline void rp_free(void *p) { heap_caps_free(p); }

size_t rp_utf8_encode(uint32_t cp, char out[4]);
/* Invalid sequences consume at least one byte and return U+FFFD. */
uint32_t rp_utf8_next(const char *s, size_t n, size_t *used);
size_t rp_entities(const char *s, size_t n, char *out, size_t cap);
bool rp_tokens(const char *list, const char *token);
/* Resolve an archive-local URL relative to a document, preserving its fragment.
 * Reject schemes, absolute external URLs, NUL escapes, traversal and truncation. */
bool rp_uri(const char *document, const char *ref, char out[RP_PATH], char fragment[128]);

typedef enum { RP_XML_EOF, RP_XML_START, RP_XML_END, RP_XML_TEXT, RP_XML_ERROR } rp_xml_kind_t;
typedef struct {
    rp_xml_kind_t kind;
    const char *raw, *end, *text;
    size_t text_len;
    char name[48]; /* local name, without namespace prefix */
    bool empty, cdata;
} rp_xml_token_t;
typedef struct {
    const char *pos, *end;
} rp_xml_t;
void rp_xml_init(rp_xml_t *x, const char *data, size_t len);
rp_xml_kind_t rp_xml_next(rp_xml_t *x, rp_xml_token_t *t);
bool rp_xml_attr(const rp_xml_token_t *t, const char *name, char *out, size_t cap);
/* Convert a BOM-marked UTF-16 XML resource to UTF-8; reject embedded NULs. */
esp_err_t rp_xml_utf8(char **data, size_t *len);
