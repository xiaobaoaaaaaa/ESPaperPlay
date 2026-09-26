/* SPDX-License-Identifier: MIT */
#include "reader_css.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define B ESPAPERPLAY_READER_BLK_BOLD
#define I ESPAPERPLAY_READER_BLK_ITALIC
#define C ESPAPERPLAY_READER_BLK_CENTER
#define R ESPAPERPLAY_READER_BLK_RIGHT
static void trim(const char **p, const char **e) {
    while (*p < *e && isspace((unsigned char)**p))
        (*p)++;
    while (*e > *p && isspace((unsigned char)(*e)[-1]))
        (*e)--;
}
static void apply(rp_style_t *to, const rp_style_t *from, unsigned mask) {
    if (mask & 1)
        to->flags = (to->flags & ~B) | (from->flags & B);
    if (mask & 2)
        to->flags = (to->flags & ~I) | (from->flags & I);
    if (mask & 4)
        to->flags = (to->flags & ~(C | R)) | (from->flags & (C | R));
    if (mask & 8)
        to->indent = from->indent;
    if (mask & 16)
        to->hidden = from->hidden;
    if (mask & 32)
        to->pre = from->pre;
}
static void declarations(const char *p, size_t len, rp_css_rule_t *rule) {
    const char *end = p + len;
    while (p < end) {
        const char *e = memchr(p, ';', end - p);
        if (!e)
            e = end;
        const char *colon = memchr(p, ':', e - p);
        if (colon) {
            const char *a = p, *ae = colon, *v = colon + 1, *ve = e;
            trim(&a, &ae);
            trim(&v, &ve);
            char key[40], value[80];
            size_t kn = ae - a, vn = ve - v;
            if (kn < sizeof(key) && vn < sizeof(value)) {
                for (size_t i = 0; i < kn; i++)
                    key[i] = tolower((unsigned char)a[i]);
                key[kn] = 0;
                for (size_t i = 0; i < vn; i++)
                    value[i] = tolower((unsigned char)v[i]);
                value[vn] = 0;
                char *bang = strchr(value, '!');
                bool important = bang && strstr(bang, "important");
                if (bang) {
                    *bang = 0;
                    while (bang > value && isspace((unsigned char)bang[-1]))
                        *--bang = 0;
                }
                rp_style_t style = {0};
                unsigned mask = 0;
                if (!strcmp(key, "font-weight")) {
                    if (!strcmp(value, "bold") || !strcmp(value, "bolder") || atoi(value) >= 600) {
                        style.flags |= B;
                        mask = 1;
                    } else if (!strcmp(value, "normal") || !strcmp(value, "lighter") ||
                               atoi(value) > 0)
                        mask = 1;
                } else if (!strcmp(key, "font-style")) {
                    if (!strcmp(value, "italic") || !strcmp(value, "oblique")) {
                        style.flags |= I;
                        mask = 2;
                    } else if (!strcmp(value, "normal"))
                        mask = 2;
                } else if (!strcmp(key, "text-align")) {
                    if (!strcmp(value, "center")) {
                        style.flags |= C;
                        mask = 4;
                    } else if (!strcmp(value, "right") || !strcmp(value, "end")) {
                        style.flags |= R;
                        mask = 4;
                    } else if (!strcmp(value, "left") || !strcmp(value, "start") ||
                               !strcmp(value, "justify"))
                        mask = 4;
                } else if (!strcmp(key, "text-indent")) {
                    char *tail;
                    double n = strtod(value, &tail);
                    if (tail != value) {
                        mask = 8;
                        style.indent = n > 0;
                    }
                } else if (!strcmp(key, "display")) {
                    if (!strcmp(value, "none")) {
                        mask = 16;
                        style.hidden = true;
                    } else if (!strcmp(value, "block") || !strcmp(value, "inline") ||
                               !strcmp(value, "inline-block"))
                        mask = 16;
                } else if (!strcmp(key, "white-space")) {
                    if (!strcmp(value, "pre") || !strcmp(value, "pre-wrap") ||
                        !strcmp(value, "break-spaces")) {
                        mask = 32;
                        style.pre = true;
                    } else if (!strcmp(value, "normal") || !strcmp(value, "nowrap"))
                        mask = 32;
                }
                if (mask && (!(rule->important & mask) || important)) {
                    apply(&rule->style, &style, mask);
                    rule->mask |= mask;
                    if (important)
                        rule->important |= mask;
                }
            }
        }
        p = e < end ? e + 1 : e;
    }
}
static void parse_css(rp_css_t *css, const char *source, size_t len, unsigned nesting) {
    if (nesting > 4)
        return;
    if (len > 65536)
        return;
    char *clean = rp_alloc(len + 1);
    if (!clean)
        return;
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        if (i + 1 < len && source[i] == '/' && source[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && (source[i] != '*' || source[i + 1] != '/'))
                i++;
            i++;
            clean[w++] = ' ';
        } else
            clean[w++] = source[i];
    }
    clean[w] = 0;
    const char *p = clean, *end = clean + w;
    while (p < end) {
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (p == end)
            break;
        if (*p == '@') {
            const char *brace = memchr(p, '{', end - p), *semi = memchr(p, ';', end - p);
            if (semi && (!brace || semi < brace)) {
                p = semi + 1;
                continue;
            }
            if (!brace)
                break;
            int depth = 1;
            const char *e = brace + 1;
            for (; e < end && depth; e++) {
                if (*e == '{')
                    depth++;
                if (*e == '}')
                    depth--;
            }
            if (depth)
                break;
            if (!strncasecmp(p, "@media", 6)) {
                char media[128];
                size_t n = brace - p - 6;
                if (n < sizeof(media)) {
                    memcpy(media, p + 6, n);
                    media[n] = 0;
                    if (!strstr(media, "print") && !strstr(media, "speech"))
                        parse_css(css, brace + 1, e - brace - 2, nesting + 1);
                }
            }
            p = e;
            continue;
        }
        const char *brace = memchr(p, '{', end - p);
        if (!brace)
            break;
        const char *close = memchr(brace + 1, '}', end - brace - 1);
        if (!close)
            break;
        rp_css_rule_t rule = {0};
        declarations(brace + 1, close - brace - 1, &rule);
        for (const char *s = p; s < brace;) {
            const char *comma = memchr(s, ',', brace - s);
            const char *e = comma ? comma : brace;
            const char *a = s;
            trim(&a, &e);
            if (rule.mask && e > a && (size_t)(e - a) < sizeof(rule.selector) &&
                css->count < RP_CSS_RULES) {
                memcpy(rule.selector, a, e - a);
                rule.selector[e - a] = 0;
                css->rules[css->count++] = rule;
            }
            s = comma ? comma + 1 : brace;
        }
        p = close + 1;
    }
    rp_free(clean);
}
void rp_css_parse(rp_css_t *css, const char *source, size_t len) { parse_css(css, source, len, 0); }
static bool compound(const char *s, size_t n, const rp_element_t *el, unsigned *specificity) {
    size_t p = 0;
    bool tested = false;
    while (p < n) {
        char prefix = 0;
        if (s[p] == '#' || s[p] == '.')
            prefix = s[p++];
        size_t start = p;
        if (!prefix && p < n && s[p] == '*') {
            p++;
            tested = true;
            continue;
        }
        while (p < n && (isalnum((unsigned char)s[p]) || s[p] == '-' || s[p] == '_' ||
                         (unsigned char)s[p] >= 128))
            p++;
        if (p == start)
            return false;
        char word[192];
        if (p - start >= sizeof(word))
            return false;
        memcpy(word, s + start, p - start);
        word[p - start] = 0;
        if (prefix == '#') {
            if (strcmp(el->id, word))
                return false;
            *specificity += 100;
        } else if (prefix == '.') {
            if (!rp_tokens(el->classes, word))
                return false;
            *specificity += 10;
        } else {
            if (strcasecmp(el->name, word))
                return false;
            *specificity += 1;
        }
        tested = true;
    }
    return tested;
}
static bool selector(const char *s, const rp_element_t *stack, int depth, unsigned *specificity) {
    const char *end = s + strlen(s);
    int at = depth - 1;
    bool direct = true;
    *specificity = 0;
    while (end > s) {
        while (end > s && isspace((unsigned char)end[-1]))
            end--;
        const char *start = end;
        while (start > s && !isspace((unsigned char)start[-1]) && start[-1] != '>')
            start--;
        if (start == end)
            return false;
        bool found = false;
        for (; at >= 0; at--) {
            unsigned score = 0;
            if (compound(start, end - start, &stack[at], &score)) {
                *specificity += score;
                at--;
                found = true;
                break;
            }
            if (direct)
                return false;
        }
        if (!found)
            return false;
        end = start;
        while (end > s && isspace((unsigned char)end[-1]))
            end--;
        direct = end > s && end[-1] == '>';
        if (direct)
            end--;
    }
    return true;
}
void rp_css_resolve(const rp_css_t *css, rp_element_t *stack, int depth, const char *inline_style) {
    rp_style_t *style = &stack[depth - 1].style;
    unsigned ranks[6] = {0};
    for (int i = 0; i <= css->count; i++) {
        rp_css_rule_t in = {0};
        const rp_css_rule_t *r;
        unsigned score;
        if (i == css->count) {
            declarations(inline_style, strlen(inline_style), &in);
            r = &in;
            score = 1000;
        } else {
            r = &css->rules[i];
            if (!selector(r->selector, stack, depth, &score))
                continue;
        }
        for (unsigned p = 0; p < 6; p++) {
            unsigned bit = 1u << p, rank = score + 1 + ((r->important & bit) ? 100000 : 0);
            if ((r->mask & bit) && rank >= ranks[p]) {
                apply(style, &r->style, bit);
                ranks[p] = rank;
            }
        }
    }
}
static void load_css(epub_zip_t *zip, const char *path, rp_css_t *css, int depth, int *loaded) {
    if (depth > 4 || *loaded >= 16)
        return;
    int zi = epub_zip_find(zip, path);
    if (zi < 0 || zip->ents[zi].usize > 65536)
        return;
    char *text = NULL;
    size_t len;
    if (rp_zip_xml(zip, zi, &text, &len) != ESP_OK)
        return;
    (*loaded)++;
    /* Imports precede local declarations; recursion/aggregate count also bounds cycles. */
    const char *p = text;
    while ((p = strstr(p, "@import"))) {
        p += 7;
        const char *end = strchr(p, ';');
        if (!end)
            break;
        const char *q = p;
        while (q < end && *q != '\'' && *q != '"' && *q != '(')
            q++;
        if (q < end) {
            char quote = *q++;
            if (quote == '(') {
                while (q < end && isspace((unsigned char)*q))
                    q++;
                if (q < end && (*q == '\'' || *q == '"'))
                    quote = *q++;
                else
                    quote = ')';
            }
            const char *e = memchr(q, quote, end - q);
            if (e && (size_t)(e - q) < RP_PATH) {
                char href[RP_PATH], resolved[RP_PATH];
                memcpy(href, q, e - q);
                href[e - q] = 0;
                if (rp_uri(path, href, resolved, NULL))
                    load_css(zip, resolved, css, depth + 1, loaded);
            }
        }
        p = end + 1;
    }
    rp_css_parse(css, text, len);
    rp_free(text);
}
esp_err_t rp_css_document(epub_zip_t *zip, const char *path, const char *html, size_t len,
                          rp_css_t *css) {
    memset(css, 0, sizeof(*css));
    rp_xml_t x;
    rp_xml_token_t t;
    rp_xml_init(&x, html, len);
    int loaded = 0;
    bool in_style = false;
    while (rp_xml_next(&x, &t) != RP_XML_EOF && t.kind != RP_XML_ERROR) {
        if (t.kind == RP_XML_START && !strcmp(t.name, "link")) {
            char rel[128], href[RP_PATH], resolved[RP_PATH];
            rp_xml_attr(&t, "rel", rel, sizeof(rel));
            if (rp_tokens(rel, "stylesheet") && rp_xml_attr(&t, "href", href, sizeof(href)) &&
                rp_uri(path, href, resolved, NULL))
                load_css(zip, resolved, css, 0, &loaded);
        }
        if (t.kind == RP_XML_START && !strcmp(t.name, "style"))
            in_style = true;
        if (t.kind == RP_XML_END && !strcmp(t.name, "style"))
            in_style = false;
        if (t.kind == RP_XML_TEXT && in_style)
            rp_css_parse(css, t.text, t.text_len);
    }
    return t.kind == RP_XML_ERROR ? ESP_ERR_INVALID_ARG : ESP_OK;
}
