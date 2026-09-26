/* SPDX-License-Identifier: MIT */
#include "reader_package.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *copy(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = rp_alloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}
void rp_package_free(rp_package_t *b) {
    for (int i = 0; i < b->item_cnt; i++) {
        rp_free(b->items[i].zip_path);
        rp_free(b->items[i].id);
        rp_free(b->items[i].fallback);
    }
    rp_free(b->items);
    rp_free(b->spine);
    rp_free(b->chapters);
    memset(b, 0, sizeof(*b));
    b->cover_entry = -1;
}
static int item_id(const rp_package_t *b, const char *id) {
    for (int i = 0; i < b->item_cnt; i++)
        if (!strcmp(b->items[i].id, id))
            return i;
    return -1;
}
static void append_label(char *out, size_t cap, const char *text, size_t len) {
    size_t used = strlen(out);
    if (used + 1 >= cap)
        return;
    char decoded[512];
    rp_entities(text, len, decoded, sizeof(decoded));
    for (const unsigned char *p = (unsigned char *)decoded; *p && used + 1 < cap; p++) {
        if (isspace(*p)) {
            if (used && out[used - 1] != ' ')
                out[used++] = ' ';
        } else
            out[used++] = *p;
    }
    /* Drop a possibly truncated final UTF-8 code point. */
    size_t valid = 0;
    while (valid < used) {
        size_t n;
        rp_utf8_next(out + valid, used - valid, &n);
        if (n == 1 && (unsigned char)out[valid] >= 128)
            break;
        valid += n;
    }
    out[valid] = 0;
}
static esp_err_t find_opf(epub_zip_t *zip, char path[RP_PATH]) {
    char *xml = NULL;
    size_t n = 0;
    esp_err_t err = rp_zip_xml(zip, epub_zip_find(zip, "META-INF/container.xml"), &xml, &n);
    if (err != ESP_OK)
        return err;
    rp_xml_t x;
    rp_xml_token_t t;
    rp_xml_init(&x, xml, n);
    bool found = false;
    while (rp_xml_next(&x, &t) != RP_XML_EOF && t.kind != RP_XML_ERROR) {
        if (t.kind != RP_XML_START || strcmp(t.name, "rootfile"))
            continue;
        char href[RP_PATH], mime[96];
        if (rp_xml_attr(&t, "full-path", href, sizeof(href)) &&
            (!rp_xml_attr(&t, "media-type", mime, sizeof(mime)) ||
             !strcmp(mime, "application/oebps-package+xml")) &&
            rp_uri("", href, path, NULL) && epub_zip_find(zip, path) >= 0) {
            found = true;
            break;
        }
    }
    rp_free(xml);
    return found ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
static uint8_t kind(const char *mime) {
    if (!strcmp(mime, "application/xhtml+xml") || !strcmp(mime, "text/html") ||
        !strcmp(mime, "image/svg+xml"))
        return 1;
    if (!strncmp(mime, "image/", 6))
        return 2;
    if (strstr(mime, "font"))
        return 3;
    if (!strcmp(mime, "text/css"))
        return 4;
    if (strstr(mime, "dtbncx"))
        return 5;
    return 0;
}
static esp_err_t parse_opf(epub_zip_t *zip, rp_package_t *b, char **ncx_id) {
    char *xml = NULL;
    size_t n = 0;
    esp_err_t err = rp_zip_xml(zip, epub_zip_find(zip, b->opf_path), &xml, &n);
    if (err != ESP_OK)
        return err;
    b->items = rp_alloc(RP_MAX_ITEMS * sizeof(*b->items));
    b->spine = rp_alloc(RP_MAX_ITEMS * sizeof(int));
    if (!b->items || !b->spine) {
        rp_free(xml);
        return ESP_ERR_NO_MEM;
    }
    rp_xml_t x;
    rp_xml_token_t t;
    rp_xml_init(&x, xml, n);
    bool manifest = false, title = false;
    char cover_id[128] = {0};
    while (rp_xml_next(&x, &t) != RP_XML_EOF) {
        if (t.kind == RP_XML_ERROR) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        if (t.kind == RP_XML_START && !strcmp(t.name, "manifest"))
            manifest = true;
        if (t.kind == RP_XML_END && !strcmp(t.name, "manifest"))
            manifest = false;
        if (t.kind == RP_XML_START && !strcmp(t.name, "title") && !b->title[0])
            title = true;
        if (t.kind == RP_XML_END && !strcmp(t.name, "title"))
            title = false;
        if (t.kind == RP_XML_TEXT && title)
            append_label(b->title, sizeof(b->title), t.text, t.text_len);
        if (t.kind != RP_XML_START)
            continue;
        char id[128], href[RP_PATH], mime[128], props[256], fallback[128];
        if (!strcmp(t.name, "meta") && rp_xml_attr(&t, "name", id, sizeof(id)) &&
            !strcmp(id, "cover"))
            rp_xml_attr(&t, "content", cover_id, sizeof(cover_id));
        if (!manifest || strcmp(t.name, "item"))
            continue;
        if (b->item_cnt >= RP_MAX_ITEMS) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (!rp_xml_attr(&t, "id", id, sizeof(id)) ||
            !rp_xml_attr(&t, "href", href, sizeof(href)) ||
            !rp_xml_attr(&t, "media-type", mime, sizeof(mime)) || item_id(b, id) >= 0) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        rp_xml_attr(&t, "properties", props, sizeof(props));
        rp_xml_attr(&t, "fallback", fallback, sizeof(fallback));
        char path[RP_PATH];
        bool local = rp_uri(b->opf_path, href, path, NULL);
        rp_item_t *it = &b->items[b->item_cnt++];
        memset(it, 0, sizeof(*it));
        it->id = copy(id);
        it->zip_path = copy(local ? path : "");
        it->fallback = copy(fallback);
        it->kind = local ? kind(mime) : 0;
        it->nav = rp_tokens(props, "nav");
        it->cover = rp_tokens(props, "cover-image");
        if (!it->id || !it->zip_path || !it->fallback) {
            err = ESP_ERR_NO_MEM;
            break;
        }
    }
    if (err == ESP_OK) {
        rp_xml_init(&x, xml, n);
        bool spine = false;
        while (rp_xml_next(&x, &t) != RP_XML_EOF && t.kind != RP_XML_ERROR) {
            if (t.kind == RP_XML_START && !strcmp(t.name, "spine")) {
                spine = true;
                char toc[128];
                if (rp_xml_attr(&t, "toc", toc, sizeof(toc))) {
                    rp_free(*ncx_id);
                    *ncx_id = copy(toc);
                }
            }
            if (t.kind == RP_XML_END && !strcmp(t.name, "spine"))
                spine = false;
            if (!spine || t.kind != RP_XML_START || strcmp(t.name, "itemref"))
                continue;
            char id[128];
            if (!rp_xml_attr(&t, "idref", id, sizeof(id))) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            int i = item_id(b, id), steps = 0;
            while (i >= 0 && b->items[i].kind != 1 && steps++ < b->item_cnt)
                i = item_id(b, b->items[i].fallback);
            if (i < 0 || steps >= b->item_cnt || !b->items[i].zip_path[0] ||
                epub_zip_find(zip, b->items[i].zip_path) < 0) {
                err = ESP_ERR_NOT_SUPPORTED;
                break;
            }
            if (b->spine_cnt >= RP_MAX_ITEMS) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            /* Include linear=no auxiliary documents so notes remain reachable. */
            b->spine[b->spine_cnt++] = i;
        }
        for (int i = 0; i < b->item_cnt; i++)
            if (b->items[i].cover || !strcmp(b->items[i].id, cover_id)) {
                b->cover_entry = epub_zip_find(zip, b->items[i].zip_path);
                if (b->cover_entry >= 0)
                    break;
            }
    }
    rp_free(xml);
    if (err == ESP_OK && !b->spine_cnt)
        err = ESP_ERR_NOT_SUPPORTED;
    return err;
}
typedef struct {
    int doc;
    uint32_t offset;
    char fragment[128], title[128];
} toc_entry_t;
typedef struct {
    toc_entry_t *entries;
    int count, capacity;
    bool exhausted;
    rp_package_t *book;
} toc_t;
static void toc_add(toc_t *toc, const char *base, const char *href, const char *label) {
    if (toc->count >= RP_MAX_CHAPTERS - 1) {
        toc->exhausted = true;
        return;
    }
    if (toc->count == toc->capacity) {
        int capacity = toc->capacity ? toc->capacity * 2 : 32;
        if (capacity > RP_MAX_CHAPTERS)
            capacity = RP_MAX_CHAPTERS;
        void *entries = rp_realloc(toc->entries, capacity * sizeof(*toc->entries));
        if (!entries) {
            toc->exhausted = true;
            return;
        }
        toc->entries = entries;
        toc->capacity = capacity;
    }
    char path[RP_PATH], fragment[128];
    if (!rp_uri(base, href, path, fragment))
        return;
    for (int i = 0; i < toc->book->spine_cnt; i++)
        if (!strcmp(path, toc->book->items[toc->book->spine[i]].zip_path)) {
            toc_entry_t *e = &toc->entries[toc->count++];
            memset(e, 0, sizeof(*e));
            e->doc = i;
            e->offset = fragment[0] ? UINT32_MAX : 0;
            strcpy(e->fragment, fragment);
            snprintf(e->title, sizeof(e->title), "%s", label);
            size_t n = strlen(e->title);
            while (n && e->title[n - 1] == ' ')
                e->title[--n] = 0;
            break;
        }
}
static void parse_nav(const char *xml, size_t len, const char *path, bool ncx, toc_t *toc) {
    typedef struct {
        char name[48], href[RP_PATH], label[128];
        bool nav, collect;
    } frame_t;
    frame_t *stack = rp_alloc(RP_MAX_DEPTH * sizeof(*stack));
    if (!stack)
        return;
    int depth = 0;
    rp_xml_t x;
    rp_xml_token_t t;
    rp_xml_init(&x, xml, len);
    while (rp_xml_next(&x, &t) != RP_XML_EOF && t.kind != RP_XML_ERROR) {
        if (t.kind == RP_XML_TEXT) {
            for (int i = depth - 1; i >= 0; i--)
                if (stack[i].collect) {
                    append_label(stack[i].label, sizeof(stack[i].label), t.text, t.text_len);
                    break;
                }
            continue;
        }
        if (t.kind == RP_XML_START) {
            if (depth == RP_MAX_DEPTH)
                break;
            frame_t *f = &stack[depth];
            memset(f, 0, sizeof(*f));
            strcpy(f->name, t.name);
            f->nav = depth && stack[depth - 1].nav;
            if (!ncx && !strcmp(t.name, "nav")) {
                char type[128];
                rp_xml_attr(&t, "type", type, sizeof(type));
                f->nav = rp_tokens(type, "toc");
            }
            if (ncx && !strcmp(t.name, "navpoint"))
                f->collect = true;
            if (ncx && !strcmp(t.name, "content"))
                for (int i = depth - 1; i >= 0; i--)
                    if (!strcmp(stack[i].name, "navpoint")) {
                        rp_xml_attr(&t, "src", stack[i].href, sizeof(stack[i].href));
                        break;
                    }
            if (!ncx && f->nav && !strcmp(t.name, "a")) {
                f->collect = true;
                rp_xml_attr(&t, "href", f->href, sizeof(f->href));
            }
            depth++;
            if (!t.empty)
                continue;
        }
        if (t.kind == RP_XML_END || t.empty) {
            int match = depth - 1;
            while (match >= 0 && strcmp(stack[match].name, t.name))
                match--;
            if (match < 0)
                continue;
            while (depth > match) {
                frame_t *f = &stack[--depth];
                if (f->collect && f->href[0])
                    toc_add(toc, path, f->href, f->label);
            }
        }
    }
    rp_free(stack);
}
static int order(const void *a, const void *b) {
    const toc_entry_t *x = a, *y = b;
    if (x->doc != y->doc)
        return x->doc < y->doc ? -1 : 1;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}
static esp_err_t build_toc(epub_zip_t *zip, rp_package_t *b, const char *ncx_id) {
    toc_t toc = {.book = b};
    int nav = -1, ncx = -1;
    for (int i = 0; i < b->item_cnt; i++) {
        if (b->items[i].nav)
            nav = i;
        if (b->items[i].kind == 5 && (ncx < 0 || (ncx_id && !strcmp(b->items[i].id, ncx_id))))
            ncx = i;
    }
    for (int pass = 0; pass < 2 && !toc.count; pass++) {
        int item = pass ? ncx : nav;
        if (item < 0)
            continue;
        char *xml = NULL;
        size_t n;
        if (rp_zip_xml(zip, epub_zip_find(zip, b->items[item].zip_path), &xml, &n) == ESP_OK) {
            parse_nav(xml, n, b->items[item].zip_path, pass == 1, &toc);
            rp_free(xml);
        }
    }
    if (toc.exhausted) {
        rp_free(toc.entries);
        return ESP_ERR_NO_MEM;
    }
    /* Resolve all fragment targets in one tokenization per referenced document. */
    for (int d = 0; d < b->spine_cnt; d++) {
        bool need = false;
        for (int i = 0; i < toc.count; i++)
            if (toc.entries[i].doc == d && toc.entries[i].fragment[0])
                need = true;
        if (!need)
            continue;
        char *xml = NULL;
        size_t n;
        if (rp_zip_xml(zip, epub_zip_find(zip, b->items[b->spine[d]].zip_path), &xml, &n) != ESP_OK)
            continue;
        rp_xml_t x;
        rp_xml_token_t t;
        rp_xml_init(&x, xml, n);
        while (rp_xml_next(&x, &t) != RP_XML_EOF && t.kind != RP_XML_ERROR) {
            if (t.kind != RP_XML_START)
                continue;
            char id[128];
            if (!rp_xml_attr(&t, "id", id, sizeof(id)) && !rp_xml_attr(&t, "name", id, sizeof(id)))
                continue;
            for (int i = 0; i < toc.count; i++)
                if (toc.entries[i].doc == d && toc.entries[i].offset == UINT32_MAX &&
                    !strcmp(toc.entries[i].fragment, id))
                    toc.entries[i].offset = t.raw - xml;
        }
        rp_free(xml);
    }
    if (toc.count)
        qsort(toc.entries, toc.count, sizeof(*toc.entries), order);
    int capacity = toc.count + 1 > b->spine_cnt ? toc.count + 1 : b->spine_cnt;
    b->chapters = rp_alloc(capacity * sizeof(*b->chapters));
    if (!b->chapters) {
        rp_free(toc.entries);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < toc.count; i++) {
        toc_entry_t *e = &toc.entries[i];
        if (e->offset == UINT32_MAX)
            continue;
        if (!b->chapter_count && (e->doc > 0 || e->offset > 0))
            b->chapters[b->chapter_count++] = (rp_chapter_t){.doc = 0};
        if (b->chapter_count && b->chapters[b->chapter_count - 1].doc == e->doc &&
            b->chapters[b->chapter_count - 1].offset == e->offset)
            continue;
        rp_chapter_t *c = &b->chapters[b->chapter_count++];
        c->doc = e->doc;
        c->offset = e->offset;
        strcpy(c->title, e->title);
    }
    if (!b->chapter_count)
        for (int d = 0; d < b->spine_cnt; d++)
            b->chapters[b->chapter_count++] = (rp_chapter_t){.doc = d};
    rp_free(toc.entries);
    return ESP_OK;
}
static esp_err_t package_read(epub_zip_t *zip, rp_package_t *b, bool toc) {
    memset(b, 0, sizeof(*b));
    b->cover_entry = -1;
    char *ncx = NULL;
    esp_err_t err = find_opf(zip, b->opf_path);
    if (err == ESP_OK)
        err = parse_opf(zip, b, &ncx);
    if (err == ESP_OK && toc)
        err = build_toc(zip, b, ncx);
    rp_free(ncx);
    if (err == ESP_OK) {
        void *items = rp_realloc(b->items, b->item_cnt * sizeof(*b->items));
        if (items)
            b->items = items;
        void *spine = rp_realloc(b->spine, b->spine_cnt * sizeof(*b->spine));
        if (spine)
            b->spine = spine;
    }
    if (err != ESP_OK)
        rp_package_free(b);
    return err;
}

esp_err_t rp_package_open(epub_zip_t *z, rp_package_t *b) { return package_read(z, b, true); }
esp_err_t rp_package_metadata(epub_zip_t *z, rp_package_t *b) { return package_read(z, b, false); }
