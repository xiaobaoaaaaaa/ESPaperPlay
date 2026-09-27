/* SPDX-License-Identifier: MIT */
#pragma once
#include "espaperplay_reader_blocks.h"
#include <stdbool.h>
#include <stddef.h>

#define ESPAPERPLAY_READER_LINE_RUNS 96
/* Rendering supplies font metrics; the line breaker has no LVGL dependency. */
typedef struct {
    int width, ascent, descent;
} espaperplay_reader_glyph_t;
typedef espaperplay_reader_glyph_t (*espaperplay_reader_measure_t)(void *ctx, uint16_t flags,
                                                                   uint16_t scale, uint32_t cp,
                                                                   uint32_t next);
typedef struct {
    uint32_t off, len;
    uint16_t flags, scale;
    int x, width, ascent;
} espaperplay_reader_fragment_t;
typedef struct {
    uint32_t next;
    int width, height, ascent, count;
    espaperplay_reader_fragment_t fragments[ESPAPERPLAY_READER_LINE_RUNS];
} espaperplay_reader_line_t;
/* Spans are sorted, non-overlapping chapter byte ranges. A zero-width box
 * still consumes one codepoint. The bounded run budget creates a line break. */
bool espaperplay_reader_layout_line(const char *text, uint32_t off, uint32_t end,
                                    const espaperplay_reader_span_t *spans, int count,
                                    uint16_t flags, uint16_t scale, int width,
                                    espaperplay_reader_measure_t measure, void *ctx,
                                    espaperplay_reader_line_t *out);
