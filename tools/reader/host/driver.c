/* SPDX-License-Identifier: MIT */
#include "espaperplay_reader_epub.h"
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
int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    if (getenv("READER_FAIL_ALLOC"))
        fail_at = strtoul(getenv("READER_FAIL_ALLOC"), NULL, 10);
    if (!strcmp(argv[1], "core"))
        core();
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
