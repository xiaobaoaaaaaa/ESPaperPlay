/* SPDX-License-Identifier: MIT */
#include "reader_text.h"
#include "reader_gb18030.inc"
#include <string.h>
#define TEXT_MAX_FILE (64u * 1024 * 1024)
#define TEXT_SEGMENT (48u * 1024)
#define TEXT_HARD_SEGMENT (64u * 1024)
#define TEXT_MAX_SEGMENTS 4096
static int byte(rp_text_t *b) {
    int c = fgetc(b->fp);
    if (c != EOF)
        b->pos++;
    return c;
}
static void unread(rp_text_t *b, int c) {
    if (c != EOF) {
        ungetc(c, b->fp);
        b->pos--;
    }
}
static uint32_t bad(rp_text_t *b) {
    b->invalid = true;
    return 0xfffd;
}
static uint32_t unit16(rp_text_t *b) {
    int a = byte(b), c = byte(b);
    if (a < 0 || c < 0)
        return UINT32_MAX;
    return b->encoding == RP_UTF16LE ? (a | c << 8) : (a << 8 | c);
}
static uint32_t next(rp_text_t *b) {
    if (b->encoding == RP_UTF16LE || b->encoding == RP_UTF16BE) {
        uint32_t a = unit16(b);
        if (a == UINT32_MAX)
            return bad(b);
        if (a >= 0xd800 && a <= 0xdbff) {
            uint32_t pos = b->pos, c = unit16(b);
            if (c >= 0xdc00 && c <= 0xdfff)
                return 0x10000 + ((a - 0xd800) << 10) + c - 0xdc00;
            if (fseek(b->fp, pos, SEEK_SET) == 0)
                b->pos = pos;
            return bad(b);
        }
        return a >= 0xdc00 && a <= 0xdfff ? bad(b) : a;
    }
    if (b->encoding == RP_UTF32LE || b->encoding == RP_UTF32BE) {
        uint32_t cp = 0;
        for (int i = 0; i < 4; i++) {
            int c = byte(b);
            if (c < 0)
                return bad(b);
            if (b->encoding == RP_UTF32LE)
                cp |= (uint32_t)c << (8 * i);
            else
                cp = (cp << 8) | c;
        }
        return cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ? bad(b) : cp;
    }
    int a = byte(b);
    if (a < 0)
        return bad(b);
    if (a < 128)
        return a;
    if (b->encoding == RP_GB18030) {
        if (a == 0x80)
            return 0x20ac;
        if (a < 0x81 || a > 0xfe)
            return bad(b);
        int c = byte(b);
        if (c >= 0x40 && c <= 0xfe && c != 0x7f)
            return gb_two[(a - 0x81) * 190 + c - 0x40 - (c > 0x7f)];
        if (c >= 0x30 && c <= 0x39) {
            int d = byte(b);
            if (d < 0x81 || d > 0xfe) {
                unread(b, d);
                return bad(b);
            }
            int e = byte(b);
            if (e < 0x30 || e > 0x39) {
                unread(b, e);
                return bad(b);
            }
            uint32_t p = ((a - 0x81) * 10 + c - 0x30) * 1260 + (d - 0x81) * 10 + e - 0x30;
            size_t lo = 0, hi = sizeof(gb_four) / sizeof(*gb_four);
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (gb_four[mid].last < p)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo < sizeof(gb_four) / sizeof(*gb_four) && p >= gb_four[lo].first)
                return gb_four[lo].codepoint + p - gb_four[lo].first;
            return bad(b);
        }
        unread(b, c);
        return bad(b);
    }
    char u[4] = {(char)a};
    int n = a >= 0xc2 && a <= 0xdf   ? 2
            : a >= 0xe0 && a <= 0xef ? 3
            : a >= 0xf0 && a <= 0xf4 ? 4
                                     : 0;
    if (!n)
        return bad(b);
    for (int i = 1; i < n; i++) {
        int c = byte(b);
        if (c < 0 || (c & 0xc0) != 0x80) {
            unread(b, c);
            return bad(b);
        }
        u[i] = c;
    }
    size_t used;
    uint32_t cp = rp_utf8_next(u, n, &used);
    return used != (size_t)n ? bad(b) : cp;
}
void rp_text_close(rp_text_t *b) {
    if (b->fp)
        fclose(b->fp);
    rp_free(b->io);
    rp_free(b->offsets);
    rp_free(b->text);
    rp_free(b->blocks);
    memset(b, 0, sizeof(*b));
    b->current = -1;
}
static esp_err_t index_book(rp_text_t *b, uint32_t start) {
    if (fseek(b->fp, start, SEEK_SET))
        return ESP_FAIL;
    b->pos = start;
    b->invalid = false;
    b->count = 1;
    b->offsets[0] = start;
    size_t decoded = 0, lines = 0;
    bool cr = false;
    while (b->pos < b->size) {
        uint32_t raw = b->pos, cp = next(b);
        if (b->pos <= raw)
            return ESP_FAIL; /* Media removed or file truncated during indexing. */
        char u[4];
        size_t n = rp_utf8_encode(cp, u);
        if (b->encoding == RP_UTF8 && start == 0 && b->invalid)
            return ESP_ERR_INVALID_ARG;
        if (decoded >= TEXT_HARD_SEGMENT || lines >= 4096) {
            if (b->count >= TEXT_MAX_SEGMENTS)
                return ESP_ERR_INVALID_SIZE;
            b->offsets[b->count++] = raw;
            decoded = 0;
            lines = 0;
            cr = false;
        }
        if (cp == '\n' && cr) {
            cr = false;
            continue;
        }
        cr = cp == '\r';
        if (cr)
            cp = '\n';
        decoded += n;
        if (cp == '\n') {
            lines++;
            if (decoded >= TEXT_SEGMENT && b->pos < b->size) {
                /* Include a following LF in the same CRLF segment. */
                if (cr && b->pos < b->size) {
                    uint32_t saved = b->pos;
                    uint32_t following = next(b);
                    if (following != '\n') {
                        if (fseek(b->fp, saved, SEEK_SET))
                            return ESP_FAIL;
                        b->pos = saved;
                    }
                }
                if (b->pos < b->size) {
                    if (b->count >= TEXT_MAX_SEGMENTS)
                        return ESP_ERR_INVALID_SIZE;
                    b->offsets[b->count++] = b->pos;
                    decoded = 0;
                    lines = 0;
                    cr = false;
                }
            }
        }
    }
    if (ferror(b->fp) || b->pos != b->size)
        return ESP_FAIL;
    b->offsets[b->count] = b->size;
    return ESP_OK;
}
esp_err_t rp_text_open(rp_text_t *b, const char *path) {
    memset(b, 0, sizeof(*b));
    b->current = -1;
    b->fp = fopen(path, "rb");
    if (!b->fp)
        return ESP_ERR_NOT_FOUND;
    b->io = rp_alloc(8192);
    if (!b->io) {
        rp_text_close(b);
        return ESP_ERR_NO_MEM;
    }
    setvbuf(b->fp, b->io, _IOFBF, 8192);
    if (fseek(b->fp, 0, SEEK_END)) {
        rp_text_close(b);
        return ESP_FAIL;
    }
    long size = ftell(b->fp);
    if (size < 0 || (unsigned long)size > TEXT_MAX_FILE) {
        rp_text_close(b);
        return ESP_ERR_INVALID_SIZE;
    }
    b->size = size;
    rewind(b->fp);
    unsigned char bom[4] = {0};
    size_t n = fread(bom, 1, 4, b->fp);
    uint32_t start = 0;
    if (n >= 4 && !memcmp(bom, "\xff\xfe\0\0", 4)) {
        b->encoding = RP_UTF32LE;
        start = 4;
    } else if (n >= 4 && !memcmp(bom, "\0\0\xfe\xff", 4)) {
        b->encoding = RP_UTF32BE;
        start = 4;
    } else if (n >= 2 && !memcmp(bom, "\xff\xfe", 2)) {
        b->encoding = RP_UTF16LE;
        start = 2;
    } else if (n >= 2 && !memcmp(bom, "\xfe\xff", 2)) {
        b->encoding = RP_UTF16BE;
        start = 2;
    } else if (n >= 3 && !memcmp(bom, "\xef\xbb\xbf", 3)) {
        b->encoding = RP_UTF8;
        start = 3;
    }
    b->offsets = rp_alloc((TEXT_MAX_SEGMENTS + 1) * sizeof(uint32_t));
    if (!b->offsets) {
        rp_text_close(b);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = index_book(b, start);
    if (err == ESP_ERR_INVALID_ARG && start == 0) {
        b->encoding = RP_GB18030;
        err = index_book(b, 0);
    }
    /* A BOM makes the encoding authoritative: malformed code units are replaced. */
    if (err == ESP_ERR_INVALID_ARG && start == 3) {
        rp_text_close(b);
        return ESP_ERR_INVALID_ARG;
    }
    if (err != ESP_OK) {
        rp_text_close(b);
        return err;
    }
    void *small = rp_realloc(b->offsets, (b->count + 1) * sizeof(uint32_t));
    if (small)
        b->offsets = small;
    err = rp_text_load(b, 0);
    if (err != ESP_OK)
        rp_text_close(b);
    return err;
}
esp_err_t rp_text_load(rp_text_t *b, int chapter) {
    if (!b->fp || chapter < 0 || chapter >= b->count)
        return ESP_ERR_INVALID_ARG;
    if (chapter == b->current)
        return b->status;
    if (fseek(b->fp, b->offsets[chapter], SEEK_SET))
        return ESP_FAIL;
    b->pos = b->offsets[chapter];
    char *text = rp_alloc(TEXT_HARD_SEGMENT + 16);
    if (!text)
        return ESP_ERR_NO_MEM;
    size_t n = 0, lines = 1;
    bool cr = false;
    while (b->pos < b->offsets[chapter + 1]) {
        uint32_t before = b->pos, cp = next(b);
        if (b->pos <= before) {
            rp_free(text);
            return ESP_FAIL;
        }
        if (cp == '\n' && cr) {
            cr = false;
            continue;
        }
        cr = cp == '\r';
        if (cr)
            cp = '\n';
        if (cp == 0)
            cp = 0xfffd;
        char u[4];
        size_t k = rp_utf8_encode(cp, u);
        if (n + k > TEXT_HARD_SEGMENT + 8) {
            rp_free(text);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(text + n, u, k);
        n += k;
        if (cp == '\n')
            lines++;
    }
    if (ferror(b->fp) || b->pos != b->offsets[chapter + 1]) {
        rp_free(text);
        return ESP_FAIL;
    }
    text[n] = 0;
    espaperplay_reader_block_t *blocks = rp_alloc(lines * sizeof(*blocks));
    if (!blocks) {
        rp_free(text);
        return ESP_ERR_NO_MEM;
    }
    size_t off = 0;
    int count = 0;
    while (off < n || !count) {
        const char *nl = memchr(text + off, '\n', n - off);
        size_t len = nl ? (size_t)(nl - text) - off : n - off;
        blocks[count++] = (espaperplay_reader_block_t){.off = off, .len = len, .image = -1};
        if (!nl)
            break;
        off += len + 1;
    }
    rp_free(b->text);
    rp_free(b->blocks);
    b->text = text;
    b->text_len = n;
    b->blocks = blocks;
    b->block_count = count;
    b->current = chapter;
    b->status = ESP_OK;
    return ESP_OK;
}
