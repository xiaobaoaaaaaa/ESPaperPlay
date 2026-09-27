/* SPDX-License-Identifier: MIT */
#include "espaperplay_reader_epub.h"
#include "espaperplay_reader_layout.h"
#include "src/parser/reader_image.h"
#include "src/parser/reader_package.h"
#include "src/parser/reader_text.h"
#include "src/parser/reader_xhtml.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static size_t live, peak, allocs, fail_at;
typedef union {
    max_align_t alignment;
    struct {
        size_t size;
    } value;
} allocation_t;
void *heap_caps_malloc(size_t n, unsigned caps) {
    (void)caps;
    if (fail_at && ++allocs == fail_at)
        return NULL;
    allocation_t *p = malloc(sizeof(*p) + n);
    if (!p)
        return NULL;
    p->value.size = n;
    live += n;
    if (live > peak)
        peak = live;
    return p + 1;
}
void heap_caps_free(void *p) {
    if (!p)
        return;
    allocation_t *a = (allocation_t *)p - 1;
    assert(live >= a->value.size);
    live -= a->value.size;
    free(a);
}
void *heap_caps_calloc(size_t n, size_t size, unsigned caps) {
    if (size && n > SIZE_MAX / size)
        return NULL;
    void *p = heap_caps_malloc(n * size, caps);
    if (p)
        memset(p, 0, n * size);
    return p;
}
void *heap_caps_realloc(void *p, size_t n, unsigned caps) {
    void *next = heap_caps_malloc(n, caps);
    if (!next)
        return NULL;
    if (p) {
        allocation_t *a = (allocation_t *)p - 1;
        memcpy(next, p, n < a->value.size ? n : a->value.size);
        heap_caps_free(p);
    }
    return next;
}
uint32_t espcache_file_token(const char *s) {
    uint32_t h = 0;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h;
}
static void json(const char *s, size_t n) {
    putchar('"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c < 32)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    putchar('"');
}
static void chapter(int idx, const char *title, const char *text, size_t len,
                    const espaperplay_reader_block_t *blocks, int count) {
    printf("{\"chapter\":%d,\"title\":", idx);
    json(title, strlen(title));
    printf(",\"text\":");
    json(text ? text : "", len);
    printf(",\"blocks\":[");
    for (int i = 0; i < count; i++) {
        assert(blocks[i].off <= len && blocks[i].len <= len - blocks[i].off);
        printf("%s[%u,%u,%u,%d]", i ? "," : "", blocks[i].off, blocks[i].len, blocks[i].flags,
               blocks[i].image);
    }
    printf("],\"layout\":[");
    for (int i = 0; i < count; i++)
        printf("%s[%u,%d,%d,%d,%u,%u]", i ? "," : "", blocks[i].scale, blocks[i].indent,
               blocks[i].before, blocks[i].after, blocks[i].line_height, blocks[i].layout);
    printf("],\"spans\":[");
    int ns = 0;
    const espaperplay_reader_span_t *spans = espaperplay_reader_epub_spans(&ns);
    for (int i = 0; i < ns; i++) {
        assert(spans[i].off <= len && spans[i].len <= len - spans[i].off);
        printf("%s[%u,%u,%u,%u]", i ? "," : "", spans[i].off, spans[i].len, spans[i].flags,
               spans[i].scale);
    }
    puts("]}");
}
static void core(void) {
    char path[RP_PATH], fragment[128];
    assert(rp_uri("OPS/Text/ch.xhtml", "../images/a%23b.png#some%20id", path, fragment));
    assert(!strcmp(path, "OPS/images/a#b.png"));
    assert(!strcmp(fragment, "some id"));
    assert(!rp_uri("a", "../../etc/passwd", path, fragment));
    assert(!rp_uri("a", "https://example.com/a", path, fragment));
    assert(!rp_uri("a", "a%00b", path, fragment));
    const char *xml =
        "<?xml version='1.0'?><n:p foo = 'a > &amp; b' xml:id=\"anchor\"><![CDATA[<>&]]></n:p>";
    rp_xml_t x;
    rp_xml_token_t t;
    rp_xml_init(&x, xml, strlen(xml));
    assert(rp_xml_next(&x, &t) == RP_XML_START);
    assert(!strcmp(t.name, "p"));
    char value[64];
    assert(rp_xml_attr(&t, "foo", value, sizeof(value)) && !strcmp(value, "a > & b"));
    assert(rp_xml_attr(&t, "id", value, sizeof(value)) && !strcmp(value, "anchor"));
    assert(rp_xml_next(&x, &t) == RP_XML_TEXT && t.cdata);
    assert(rp_xml_next(&x, &t) == RP_XML_END);
    assert(rp_xml_next(&x, &t) == RP_XML_EOF);
    puts("{\"core\":true}");
}
static espaperplay_reader_glyph_t fixed_measure(void *ctx, uint16_t flags, uint16_t scale,
                                                uint32_t cp, uint32_t next) {
    (void)ctx;
    (void)flags;
    (void)cp;
    (void)next;
    return (espaperplay_reader_glyph_t){
        .width = scale / 10, .ascent = scale / 10, .descent = scale / 20};
}
static void layout_test(void) {
    espaperplay_reader_line_t l;
    const espaperplay_reader_span_t s[] = {{0, 2, 0, 100}, {2, 2, 1, 200}, {4, 2, 0, 100}};
    assert(
        espaperplay_reader_layout_line("abcdef", 0, 6, s, 3, 0, 100, 60, fixed_measure, NULL, &l));
    assert(l.next == 4 && l.width == 60 && l.height == 30 && l.count == 2);
    assert(l.fragments[1].x == 20 && l.fragments[1].ascent == 20);
    assert(espaperplay_reader_layout_line("abcdef", l.next, 6, s, 3, 0, 100, 60, fixed_measure,
                                          NULL, &l));
    assert(l.next == 6 && l.width == 20 && l.height == 15);
    assert(espaperplay_reader_layout_line("one two", 0, 7, NULL, 0, 0, 100, 50, fixed_measure, NULL,
                                          &l));
    assert(l.next == 4 && l.width == 40); /* keep the second word whole */
    assert(espaperplay_reader_layout_line("a\nb", 0, 3, NULL, 0, 0, 100, 100, fixed_measure, NULL,
                                          &l));
    assert(l.next == 2 && l.count == 1 && l.fragments[0].len == 1);
    assert(espaperplay_reader_layout_line("\n", 0, 1, NULL, 0, 0, 100, 0, fixed_measure, NULL, &l));
    assert(l.next == 1 && l.count == 0 && l.height == 15);
    assert(espaperplay_reader_layout_line("中", 0, 3, NULL, 0, 0, 100, 0, fixed_measure, NULL, &l));
    assert(l.next == 3);
    char many[200];
    memset(many, 'a', sizeof(many));
    espaperplay_reader_span_t runs[200];
    for (int i = 0; i < 200; i++)
        runs[i] = (espaperplay_reader_span_t){i, 1, i % 2, 100};
    uint32_t off = 0;
    while (off < 200) {
        assert(espaperplay_reader_layout_line(many, off, 200, runs, 200, 0, 100, 10000,
                                              fixed_measure, NULL, &l));
        assert(l.next > off && l.count <= ESPAPERPLAY_READER_LINE_RUNS);
        off = l.next;
    }
    puts("{\"layout\":true}");
}
int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    if (getenv("READER_FAIL_ALLOC"))
        fail_at = strtoul(getenv("READER_FAIL_ALLOC"), NULL, 10);
    if (!strcmp(argv[1], "core"))
        core();
    else if (!strcmp(argv[1], "layout"))
        layout_test();
    else if (argc >= 3 && !strcmp(argv[1], "truncated-txt")) {
        rp_text_t book;
        int err = rp_text_open(&book, argv[2]);
        assert(err == ESP_OK && book.count > 1);
        assert(truncate(argv[2], 0) == 0);
        err = rp_text_load(&book, book.count - 1);
        printf("{\"error\":%d}\n", err);
        rp_text_close(&book);
    } else if (argc >= 3 && !strcmp(argv[1], "image-lifetime")) {
        assert(espaperplay_reader_epub_open(argv[2]) == ESP_OK);
        assert(espaperplay_reader_epub_load_chapter(0) == ESP_OK);
        const lv_image_dsc_t *d = NULL;
        assert(espaperplay_reader_epub_image(0, 240, 320, &d) == ESP_OK);
        lv_image_dsc_t saved = *d;
        uint8_t pixels[8];
        assert(d->data_size == sizeof(pixels));
        memcpy(pixels, d->data, sizeof(pixels));
        /* Background pagination replaces the resident chapter, while LVGL
         * still holds the image descriptor from the visible page. */
        assert(espaperplay_reader_epub_poll_chapter(1));
        assert(d->data == saved.data && d->data_size == saved.data_size);
        assert(!memcmp(saved.data, pixels, sizeof(pixels)));
        assert(espaperplay_reader_epub_load_chapter(0) == ESP_OK);
        assert(espaperplay_reader_epub_image(0, 240, 320, &d) == ESP_OK);
        assert(d->data == saved.data);
        /* Both chapters use local image id zero, but different ZIP entries. */
        assert(espaperplay_reader_epub_load_chapter(1) == ESP_OK);
        assert(espaperplay_reader_epub_image(0, 240, 320, &d) == ESP_OK);
        assert(d->data_size == sizeof(pixels));
        assert(memcmp(d->data, pixels, sizeof(pixels)));
        espaperplay_reader_epub_close();
        puts("{\"image_lifetime\":true}");
    } else if (argc >= 3 && !strcmp(argv[1], "cover")) {
        lv_image_dsc_t d = {0};
        uint8_t *pixels = NULL;
        int err = espaperplay_reader_epub_probe_cover(argv[2], 78, 104, &d, &pixels);
        printf("{\"error\":%d,\"w\":%u,\"h\":%u}\n", err, d.header.w, d.header.h);
        heap_caps_free(pixels);
    } else if (argc >= 3 && !strcmp(argv[1], "image")) {
        FILE *f = fopen(argv[2], "rb");
        assert(f);
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        rewind(f);
        uint8_t *data = heap_caps_malloc(n, 0);
        assert(data);
        assert(fread(data, 1, n, f) == (size_t)n);
        fclose(f);
        epub_img_out_t img = {0};
        int err = rp_image_decode(argv[2], data, n, 240, 320, &img);
        printf("{\"error\":%d,\"w\":%d,\"h\":%d,\"pixels\":[", err, img.w, img.h);
        for (int i = 0; img.buf && i < img.w * img.h && i < 8; i++)
            printf("%s%u", i ? "," : "", ((uint16_t *)img.buf)[i]);
        puts("]}");
        heap_caps_free(data);
        heap_caps_free(img.buf);
    } else if (argc >= 3 && !strcmp(argv[1], "txt")) {
        rp_text_t book;
        int err = rp_text_open(&book, argv[2]);
        printf("{\"open\":%d,\"count\":%d,\"encoding\":%d}\n", err, book.count, book.encoding);
        if (err == ESP_OK) {
            for (int i = 0; i < book.count; i++) {
                err = rp_text_load(&book, i);
                if (err) {
                    printf("{\"error\":%d}\n", err);
                    break;
                }
                chapter(i, "", book.text, book.text_len, book.blocks, book.block_count);
            }
            rp_text_close(&book);
        }
    } else if (argc >= 3 && !strcmp(argv[1], "epub")) {
        int err = espaperplay_reader_epub_open(argv[2]);
        printf("{\"open\":%d,\"count\":%d}\n", err, espaperplay_reader_epub_chapter_count());
        if (err == ESP_OK) {
            for (int i = 0; i < espaperplay_reader_epub_chapter_count(); i++) {
                err = espaperplay_reader_epub_load_chapter(i);
                if (err) {
                    printf("{\"chapter\":%d,\"error\":%d}\n", i, err);
                    continue;
                }
                size_t len;
                int count;
                const char *s = espaperplay_reader_epub_chapter_text(&len);
                const espaperplay_reader_block_t *b = espaperplay_reader_epub_blocks(&count);
                chapter(i, espaperplay_reader_epub_chapter_title(), s, len, b, count);
                if (getenv("READER_LAYOUT")) {
                    int ns = 0;
                    const espaperplay_reader_span_t *spans = espaperplay_reader_epub_spans(&ns);
                    for (int j = 0; j < count; j++) {
                        uint32_t off = b[j].off, end = off + b[j].len;
                        while (off < end) {
                            espaperplay_reader_line_t line;
                            assert(espaperplay_reader_layout_line(s, off, end, spans, ns,
                                                                  b[j].flags, b[j].scale, 224,
                                                                  fixed_measure, NULL, &line));
                            assert(line.next > off && line.next <= end);
                            if (line.next < end)
                                assert(((unsigned char)s[line.next] & 0xc0) != 0x80);
                            off = line.next;
                        }
                    }
                }
                if (getenv("READER_IMAGES"))
                    for (int j = 0; j < count; j++)
                        if (b[j].image >= 0) {
                            const lv_image_dsc_t *d = NULL;
                            int e = espaperplay_reader_epub_image(b[j].image, 896, 1480, &d);
                            printf("{\"image\":%d,\"error\":%d,\"w\":%u,\"h\":%u}\n", b[j].image, e,
                                   d ? d->header.w : 0, d ? d->header.h : 0);
                        }
            }
        }
        espaperplay_reader_epub_close();
    } else
        return 2;
    fprintf(stderr, "peak_parser_bytes=%zu live_parser_bytes=%zu\n", peak, live);
    assert(!live);
    return 0;
}
