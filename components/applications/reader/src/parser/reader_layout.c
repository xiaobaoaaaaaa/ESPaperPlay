/* SPDX-License-Identifier: MIT */
#include "espaperplay_reader_layout.h"
#include "reader_parse.h"
#include <string.h>
static int locate(const espaperplay_reader_span_t *s, int count, uint32_t off) {
    int lo = 0, hi = count;
    while (lo < hi) {
        int m = lo + (hi - lo) / 2;
        if (s[m].off + s[m].len <= off)
            lo = m + 1;
        else
            hi = m;
    }
    return lo;
}
static void style(const espaperplay_reader_span_t *s, int n, int *i, uint32_t p, uint16_t *flags,
                  uint16_t *scale) {
    while (*i < n && s[*i].off + s[*i].len <= p)
        (*i)++;
    if (*i < n && s[*i].off <= p) {
        *flags = s[*i].flags;
        *scale = s[*i].scale;
    }
}
static bool closing(uint32_t c) {
    return c == 0x3001 || c == 0x3002 || c == 0xff0c || c == 0xff01 || c == 0xff1f || c == 0xff1a ||
           c == 0xff1b || c == 0x300d || c == 0x300f || c == 0x3011 || c == 0xff09;
}
static bool opening(uint32_t c) { return c == 0x300c || c == 0x300e || c == 0x3010 || c == 0xff08; }
bool espaperplay_reader_layout_line(const char *text, uint32_t off, uint32_t end,
                                    const espaperplay_reader_span_t *spans, int count,
                                    uint16_t flags, uint16_t scale, int width,
                                    espaperplay_reader_measure_t measure, void *ctx,
                                    espaperplay_reader_line_t *out) {
    memset(out, 0, sizeof(*out));
    out->next = off;
    if (!text || off >= end || !measure || count < 0 || (count && !spans))
        return false;
    uint32_t stop = end, p = off, preferred = off;
    int at = locate(spans, count, off), used_width = 0, runs = 0;
    uint16_t previous_flags = 0, previous_scale = 0;
    /* Find a break, respecting explicit newlines and common Latin/CJK break opportunities. */
    while (p < end) {
        uint16_t f = flags, z = scale ? scale : 100;
        style(spans, count, &at, p, &f, &z);
        size_t n, nn;
        uint32_t cp = rp_utf8_next(text + p, end - p, &n);
        uint32_t next = p + n < end ? rp_utf8_next(text + p + n, end - p - n, &nn) : 0;
        if (cp == '\n') {
            stop = p + n;
            break;
        }
        bool new_run = !runs || f != previous_flags || z != previous_scale;
        if (new_run && runs == ESPAPERPLAY_READER_LINE_RUNS) {
            stop = p;
            break;
        }
        uint16_t nf = flags, nz = scale ? scale : 100;
        int na = at;
        style(spans, count, &na, p + n, &nf, &nz);
        espaperplay_reader_glyph_t g = measure(ctx, f, z, cp, nf == f && nz == z ? next : 0);
        if (used_width + g.width > width && p > off) {
            stop = preferred > off ? preferred : p;
            break;
        }
        used_width += g.width;
        if (new_run) {
            runs++;
            previous_flags = f;
            previous_scale = z;
        }
        p += n;
        if (cp == ' ' || cp == '\t' || cp == '-' ||
            (cp >= 0x2e80 && !opening(cp) && !closing(next)))
            preferred = p;
    }
    /* Build only the accepted line; discarded tall glyphs cannot inflate its height. */
    at = locate(spans, count, off);
    p = off;
    int descent = 0;
    while (p < stop) {
        uint16_t f = flags, z = scale ? scale : 100;
        style(spans, count, &at, p, &f, &z);
        size_t n, nn;
        uint32_t cp = rp_utf8_next(text + p, stop - p, &n);
        uint32_t next = p + n < stop ? rp_utf8_next(text + p + n, stop - p - n, &nn) : 0;
        uint16_t nf = flags, nz = scale ? scale : 100;
        int na = at;
        style(spans, count, &na, p + n, &nf, &nz);
        espaperplay_reader_glyph_t g =
            measure(ctx, f, z, cp == '\n' ? ' ' : cp, nf == f && nz == z ? next : 0);
        if (g.ascent > out->ascent)
            out->ascent = g.ascent;
        if (g.descent > descent)
            descent = g.descent;
        if (cp != '\n') {
            espaperplay_reader_fragment_t *r = out->count ? &out->fragments[out->count - 1] : NULL;
            if (!r || r->flags != f || r->scale != z) {
                r = &out->fragments[out->count++];
                *r = (espaperplay_reader_fragment_t){
                    .off = p, .flags = f, .scale = z, .x = out->width, .ascent = g.ascent};
            }
            r->len += n;
            r->width += g.width;
            out->width += g.width;
        }
        p += n;
    }
    out->next = stop;
    out->height = out->ascent + descent;
    return stop > off;
}
