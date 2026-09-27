/* SPDX-License-Identifier: MIT */
#include "reader_xhtml.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define B ESPAPERPLAY_READER_BLK_BOLD
#define I ESPAPERPLAY_READER_BLK_ITALIC
#define Q ESPAPERPLAY_READER_BLK_QUOTE
#define C ESPAPERPLAY_READER_BLK_CENTER
#define R ESPAPERPLAY_READER_BLK_RIGHT
void rp_content_free(rp_content_t *c) {
    rp_free(c->text);
    rp_free(c->blocks);
    rp_free(c->images);
    rp_free(c->spans);
    memset(c, 0, sizeof(*c));
}
static bool reserve(void **p, size_t size, int *cap, int need, int max) {
    if (need > max)
        return false;
    if (need <= *cap)
        return true;
    int n = *cap ? *cap * 2 : 64;
    if (n < need)
        n = need;
    if (n > max)
        n = max;
    void *next = rp_realloc(*p, (size_t)n * size);
    if (!next)
        return false;
    *p = next;
    *cap = n;
    return true;
}
static bool put(rp_content_t *c, const char *s, size_t len) {
    if (len > RP_MAX_TEXT - c->text_len)
        return false;
    size_t need = c->text_len + len + 1;
    if (need > c->text_cap) {
        size_t n = c->text_cap ? c->text_cap * 2 : 4096;
        if (n < need)
            n = need;
        if (n > RP_MAX_TEXT + 1)
            n = RP_MAX_TEXT + 1;
        char *p = rp_realloc(c->text, n);
        if (!p)
            return false;
        c->text = p;
        c->text_cap = n;
    }
    if (len) {
        espaperplay_reader_span_t *last = c->span_count ? &c->spans[c->span_count - 1] : NULL;
        uint16_t scale = c->text_scale ? c->text_scale : 100;
        if (last && last->off + last->len == c->text_len && last->flags == c->text_flags &&
            last->scale == scale)
            last->len += len;
        else {
            if (!reserve((void **)&c->spans, sizeof(*c->spans), &c->span_cap, c->span_count + 1,
                         RP_MAX_SPANS))
                return false;
            c->spans[c->span_count++] = (espaperplay_reader_span_t){
                .off = c->text_len, .len = len, .flags = c->text_flags, .scale = scale};
        }
    }
    memcpy(c->text + c->text_len, s, len);
    c->text_len += len;
    c->text[c->text_len] = 0;
    return true;
}
static bool block(rp_content_t *c, size_t off, size_t len, uint16_t flags, int image) {
    if (!reserve((void **)&c->blocks, sizeof(*c->blocks), &c->block_cap, c->block_count + 1,
                 RP_MAX_BLOCKS))
        return false;
    c->blocks[c->block_count++] =
        (espaperplay_reader_block_t){.off = off, .len = len, .flags = flags, .image = image};
    return true;
}
static bool flush(rp_content_t *c) {
    c->pending_space = false;
    if (c->text_len > c->paragraph &&
        !block(c, c->paragraph, c->text_len - c->paragraph, c->paragraph_flags, -1))
        return false;
    if (c->text_len > c->paragraph) {
        espaperplay_reader_block_t *b = &c->blocks[c->block_count - 1];
        b->scale = c->paragraph_style.scale;
        b->indent = c->paragraph_style.indent;
        b->before = c->paragraph_style.before;
        b->after = c->paragraph_style.after;
        b->line_height = c->paragraph_style.line_height;
        b->layout = c->paragraph_style.layout;
    }
    c->paragraph = c->text_len;
    c->paragraph_flags = 0;
    return true;
}
static bool is_block(const char *name) {
    static const char *tags[] = {
        "p",  "div", "section", "article",    "header",     "footer", "main", "aside",  "h1", "h2",
        "h3", "h4",  "h5",      "h6",         "blockquote", "pre",    "li",   "ul",     "ol", "dl",
        "dt", "dd",  "figure",  "figcaption", "table",      "tr",     "hr",   "address"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(*tags); i++)
        if (!strcmp(name, tags[i]))
            return true;
    return false;
}
static bool is_void(const char *name) {
    return !strcmp(name, "br") || !strcmp(name, "img") || !strcmp(name, "hr") ||
           !strcmp(name, "link") || !strcmp(name, "meta") || !strcmp(name, "input") ||
           !strcmp(name, "source") || !strcmp(name, "wbr");
}
static bool begin_text(rp_content_t *c, rp_element_t *stack, int depth) {
    if (c->text_len != c->paragraph)
        return true;
    rp_style_t style = {0};
    for (int i = depth - 1; i >= 0; i--)
        if (stack[i].block) {
            style = stack[i].style;
            break;
        }
    c->paragraph_flags = style.flags;
    c->paragraph_style = style;
    for (int i = depth - 1; i >= 0; i--)
        if (!strcmp(stack[i].name, "li") && stack[i].list_number) {
            char prefix[32];
            if (stack[i].list_number < 0)
                strcpy(prefix, "• ");
            else
                snprintf(prefix, sizeof(prefix), "%d. ", stack[i].list_number);
            stack[i].list_number = 0;
            if (!put(c, prefix, strlen(prefix)))
                return false;
            break;
        }
    return true;
}
static bool codepoint(rp_content_t *c, rp_element_t *stack, int depth, uint32_t cp) {
    const rp_style_t style = depth ? stack[depth - 1].style : (rp_style_t){.scale = 100};
    c->text_flags = style.flags;
    c->text_scale = style.scale;
    bool pre = style.pre;
    if (!pre && (cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t' || cp == '\f')) {
        if (c->text_len > c->paragraph)
            c->pending_space = true;
        return true;
    }
    if (cp == 0xad || cp == 0x200b)
        return true;
    if (!begin_text(c, stack, depth))
        return false;
    if (c->pending_space) {
        if (!put(c, " ", 1))
            return false;
        c->pending_space = false;
    }
    if (pre && cp == '\r')
        return true; /* XML CRLF commonly appears only outside pre; standalone CR handled below. */
    if (pre && cp == '\t')
        return put(c, "    ", 4);
    char u[4];
    size_t n = rp_utf8_encode(cp, u);
    return put(c, u, n);
}
static bool text(rp_content_t *c, rp_element_t *stack, int depth, const char *s, size_t n,
                 bool cdata) {
    size_t i = 0;
    while (i < n) {
        if (!cdata && s[i] == '&') {
            size_t e = i + 1;
            while (e < n && e - i < 32 && s[e] != ';' && !isspace((unsigned char)s[e]))
                e++;
            if (e < n && s[e] == ';') {
                char decoded[128];
                size_t len = rp_entities(s + i, e - i + 1, decoded, sizeof(decoded));
                size_t at = 0;
                while (at < len) {
                    size_t used;
                    uint32_t cp = rp_utf8_next(decoded + at, len - at, &used);
                    if (!codepoint(c, stack, depth, cp))
                        return false;
                    at += used;
                }
                i = e + 1;
                continue;
            }
        }
        size_t used;
        uint32_t cp = rp_utf8_next(s + i, n - i, &used);
        if (cp == '\r') {
            cp = '\n';
            if (i + used < n && s[i + used] == '\n')
                used++;
        }
        if (!codepoint(c, stack, depth, cp))
            return false;
        i += used;
    }
    return true;
}
static bool image(rp_content_t *c, epub_zip_t *zip, const char *path, const rp_xml_token_t *t,
                  rp_element_t *stack, int depth) {
    char href[RP_PATH], resolved[RP_PATH], alt[512];
    rp_xml_attr(t, "alt", alt, sizeof(alt));
    bool source = rp_xml_attr(t, "src", href, sizeof(href)) ||
                  rp_xml_attr(t, "href", href, sizeof(href)) ||
                  rp_xml_attr(t, "data", href, sizeof(href));
    int zi = source && rp_uri(path, href, resolved, NULL) ? epub_zip_find(zip, resolved) : -1;
    const char *ext = zi >= 0 ? strrchr(resolved, '.') : NULL;
    bool supported =
        ext && (!strcasecmp(ext, ".png") || !strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"));
    if (zi >= 0 && supported) {
        if (!flush(c))
            return false;
        int id = 0;
        for (; id < c->image_count; id++)
            if (c->images[id] == zi)
                break;
        if (id == c->image_count) {
            if (!reserve((void **)&c->images, sizeof(int), &c->image_cap, id + 1, RP_MAX_IMAGES))
                return false;
            c->images[c->image_count++] = zi;
        }
        return block(c, 0, 0, C, id);
    }
    if (!alt[0])
        strcpy(alt, "[图片]");
    return text(c, stack, depth, alt, strlen(alt), true);
}
esp_err_t rp_content_document(epub_zip_t *zip, const char *path, const char *html, size_t len,
                              uint32_t start, uint32_t end, rp_content_t *c) {
    rp_css_t *css = rp_alloc(sizeof(*css));
    rp_element_t *stack = rp_alloc(RP_MAX_DEPTH * sizeof(*stack));
    if (!css || !stack) {
        rp_free(css);
        rp_free(stack);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = rp_css_document(zip, path, html, len, css);
    if (err != ESP_OK)
        goto done;
    rp_xml_t x;
    rp_xml_token_t t;
    rp_xml_init(&x, html, len);
    int depth = 0;
    bool in_title = false;
    while (rp_xml_next(&x, &t) != RP_XML_EOF) {
        if (t.kind == RP_XML_ERROR) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        uint32_t offset = t.raw - html;
        bool active = offset >= start && offset < end;
        if (t.kind == RP_XML_TEXT) {
            if (in_title && !c->title[0])
                rp_entities(t.text, t.text_len, c->title, sizeof(c->title));
            if (active && (!depth || !stack[depth - 1].skip) &&
                !text(c, stack, depth, t.text, t.text_len, t.cdata)) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            continue;
        }
        if (t.kind == RP_XML_START) {
            if (depth >= RP_MAX_DEPTH) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            rp_element_t *el = &stack[depth];
            memset(el, 0, sizeof(*el));
            strcpy(el->name, t.name);
            if (depth) {
                el->style = stack[depth - 1].style;
                el->skip = stack[depth - 1].skip;
            }
            if (!el->style.scale)
                el->style.scale = 100;
            /* Margins belong to each box, unlike font/indent/line-height. */
            el->style.before = el->style.after = 0;
            el->style.layout = 0;
            el->style.hidden = false;
            el->block = is_block(t.name);
            if (el->block)
                el->style.flags &= ~ESPAPERPLAY_READER_BLK_HEAD_MASK;
            rp_xml_attr(&t, "id", el->id, sizeof(el->id));
            rp_xml_attr(&t, "class", el->classes, sizeof(el->classes));
            if (!strcmp(t.name, "head") || !strcmp(t.name, "script") || !strcmp(t.name, "style") ||
                !strcmp(t.name, "rp"))
                el->skip = true;
            if (!strcmp(t.name, "title")) {
                in_title = true;
                el->skip = true;
            }
            if (!strcmp(t.name, "b") || !strcmp(t.name, "strong") || !strcmp(t.name, "th"))
                el->style.flags |= B;
            if (!strcmp(t.name, "i") || !strcmp(t.name, "em"))
                el->style.flags |= I;
            if (!strcmp(t.name, "blockquote") || !strcmp(t.name, "dd"))
                el->style.flags |= Q;
            if (!strcmp(t.name, "center"))
                el->style.flags |= C;
            if (t.name[0] == 'h' && t.name[1] >= '1' && t.name[1] <= '6' && !t.name[2])
                el->style.flags |= B | ((t.name[1] - '0') << ESPAPERPLAY_READER_BLK_HEAD_SHIFT);
            if (t.name[0] == 'h' && t.name[1] >= '1' && t.name[1] <= '6' && !t.name[2]) {
                static const uint16_t sizes[] = {160, 140, 120, 100, 100, 100};
                el->style.scale = sizes[t.name[1] - '1'];
            }
            if (!strcmp(t.name, "small"))
                el->style.scale = el->style.scale * 80 / 100;
            if (!strcmp(t.name, "big"))
                el->style.scale = el->style.scale * 120 / 100;
            if (el->style.scale < 50)
                el->style.scale = 50;
            if (el->style.scale > 200)
                el->style.scale = 200;
            if (!strcmp(t.name, "u"))
                el->style.flags |= ESPAPERPLAY_READER_BLK_UNDERLINE;
            if (!strcmp(t.name, "s") || !strcmp(t.name, "del") || !strcmp(t.name, "strike"))
                el->style.flags |= ESPAPERPLAY_READER_BLK_STRIKE;
            if (!strcmp(t.name, "pre"))
                el->style.pre = true;
            char inline_style[1024], align[24];
            rp_xml_attr(&t, "style", inline_style, sizeof(inline_style));
            if (rp_xml_attr(&t, "align", align, sizeof(align))) {
                if (!strcasecmp(align, "center"))
                    el->style.flags = (el->style.flags & ~R) | C;
                else if (!strcasecmp(align, "right"))
                    el->style.flags = (el->style.flags & ~C) | R;
            }
            depth++;
            rp_css_resolve(css, stack, depth, inline_style);
            el->skip |= el->style.hidden;
            if (!strcmp(t.name, "ol")) {
                char num[16];
                el->list_number = rp_xml_attr(&t, "start", num, sizeof(num)) ? atoi(num) : 1;
                if (el->list_number < 1 || el->list_number > 1000000)
                    el->list_number = 1;
            }
            if (!strcmp(t.name, "li")) {
                el->list_number = -1;
                for (int i = depth - 2; i >= 0; i--)
                    if (!strcmp(stack[i].name, "ol") || !strcmp(stack[i].name, "ul")) {
                        if (!strcmp(stack[i].name, "ol"))
                            el->list_number = stack[i].list_number++;
                        break;
                    }
            }
            if (active && !el->skip) {
                if (el->block && !flush(c)) {
                    err = ESP_ERR_NO_MEM;
                    break;
                }
                if (!strcmp(t.name, "br") && (!begin_text(c, stack, depth) || !put(c, "\n", 1))) {
                    err = ESP_ERR_NO_MEM;
                    break;
                }
                if (!strcmp(t.name, "rt") && !text(c, stack, depth, "（", 3, true)) {
                    err = ESP_ERR_NO_MEM;
                    break;
                }
                if ((!strcmp(t.name, "td") || !strcmp(t.name, "th")) &&
                    c->text_len > c->paragraph && !put(c, " | ", 3)) {
                    err = ESP_ERR_NO_MEM;
                    break;
                }
                if ((!strcmp(t.name, "img") || !strcmp(t.name, "image") ||
                     !strcmp(t.name, "object")) &&
                    !image(c, zip, path, &t, stack, depth)) {
                    err = ESP_ERR_NO_MEM;
                    break;
                }
            }
            if (!t.empty && !is_void(t.name))
                continue;
        }
        if (t.kind == RP_XML_END || t.empty || is_void(t.name)) {
            int at = depth - 1;
            while (at >= 0 && strcmp(stack[at].name, t.name))
                at--;
            if (at < 0)
                continue;
            if (!strcmp(t.name, "title"))
                in_title = false;
            while (depth > at) {
                rp_element_t *el = &stack[depth - 1];
                if (active && !el->skip) {
                    if (!strcmp(el->name, "rt") && !text(c, stack, depth, "）", 3, true)) {
                        err = ESP_ERR_NO_MEM;
                        break;
                    }
                    if (el->block && !flush(c)) {
                        err = ESP_ERR_NO_MEM;
                        break;
                    }
                }
                depth--;
            }
            if (err != ESP_OK)
                break;
        }
    }
    if (err == ESP_OK && !flush(c))
        err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && !c->text && !put(c, "", 0))
        err = ESP_ERR_NO_MEM;
done:
    rp_free(css);
    rp_free(stack);
    return err;
}
