/* SPDX-License-Identifier: MIT */
#include "reader_css.h"
#include <ctype.h>
#include <math.h>
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
static int clamp(int n, int lo, int hi) { return n < lo ? lo : n > hi ? hi : n; }
static bool length(const char *v, rp_length_t *out, bool percent) {
    char *end;
    double n = strtod(v, &end);
    if (end == v || !isfinite(n) || n < -10000 || n > 10000)
        return false;
    uint8_t unit;
    if (!strcmp(end, "em") || (!*end && n == 0))
        unit = 0;
    else if (!strcmp(end, "rem"))
        unit = 1;
    else if (!strcmp(end, "px"))
        unit = 2;
    else if (!strcmp(end, "pt")) {
        unit = 2;
        n *= 4.0 / 3.0;
    } else if (percent && !strcmp(end, "%")) {
        unit = 0;
        n /= 100;
    } else
        return false;
    out->value = (int16_t)clamp((int)(n * 100), -30000, 30000);
    out->unit = unit;
    return true;
}
/* Convert to 1/64 of the user-selected root font; CSS px uses a 20px root.
 * Reader zoom thus scales author dimensions along with the text. */
static int metric(rp_length_t v, int scale) {
    int n = v.value;
    if (v.unit == 2)
        return n * 64 / 2000;
    return n * 64 * (v.unit == 1 ? 100 : scale) / 10000;
}
static void apply(rp_style_t *to, const rp_style_t *from, unsigned mask) {
    if (mask & 1)
        to->flags = (to->flags & ~B) | (from->flags & B);
    if (mask & 2)
        to->flags = (to->flags & ~I) | (from->flags & I);
    if (mask & 4)
        to->flags = (to->flags & ~(C | R)) | (from->flags & (C | R));
    if (mask & 8)
        to->indent_value = from->indent_value;
    if (mask & 16)
        to->hidden = from->hidden;
    if (mask & 32)
        to->pre = from->pre;
    if (mask & 64)
        to->size_value = from->size_value;
    if (mask & 128)
        to->line_value = from->line_value;
    if (mask & 256)
        to->before_value = from->before_value;
    if (mask & 512)
        to->after_value = from->after_value;
    if (mask & 1024) {
        const unsigned d = ESPAPERPLAY_READER_BLK_UNDERLINE | ESPAPERPLAY_READER_BLK_STRIKE;
        to->flags = (to->flags & ~d) | (from->flags & d);
    }
    to->inherit = (to->inherit & ~mask) | (from->inherit & mask);
    to->initial = (to->initial & ~mask) | (from->initial & mask);
}
static unsigned property(const char *key) {
    static const char *names[] = {"font-weight", "font-style",    "text-align",     "text-indent",
                                  "display",     "white-space",   "font-size",      "line-height",
                                  "margin-top",  "margin-bottom", "text-decoration"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (!strcmp(key, names[i]))
            return 1u << i;
    return 0;
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
            char key[40], value[128];
            size_t kn = ae - a, vn = ve - v;
            if (kn < sizeof(key) && vn < sizeof(value)) {
                for (size_t i = 0; i < kn; i++)
                    key[i] = tolower((unsigned char)a[i]);
                key[kn] = 0;
                for (size_t i = 0; i < vn; i++)
                    value[i] = tolower((unsigned char)v[i]);
                value[vn] = 0;
                char *bang = strchr(value, '!');
                bool important = false;
                if (bang) {
                    const char *q = bang + 1;
                    while (isspace((unsigned char)*q))
                        q++;
                    important = !strcmp(q, "important");
                    *bang = 0;
                    while (bang > value && isspace((unsigned char)bang[-1]))
                        *--bang = 0;
                }
                rp_style_t s = {0};
                unsigned mask = property(key);
                bool valid = true;
                if (mask && !strcmp(value, "inherit"))
                    s.inherit = mask;
                else if (mask && (!strcmp(value, "initial") || !strcmp(value, "unset"))) {
                    if (!strcmp(value, "unset") && (mask & (1 | 2 | 4 | 8 | 32 | 64 | 128)))
                        s.inherit = mask;
                    else
                        s.initial = mask;
                } else if (mask == 1) {
                    if (!strcmp(value, "bold") || !strcmp(value, "bolder") || atoi(value) >= 600)
                        s.flags |= B;
                    else
                        valid = !strcmp(value, "normal") || !strcmp(value, "lighter") ||
                                atoi(value) > 0;
                } else if (mask == 2) {
                    if (!strcmp(value, "italic") || !strcmp(value, "oblique"))
                        s.flags |= I;
                    else
                        valid = !strcmp(value, "normal");
                } else if (mask == 4) {
                    if (!strcmp(value, "center"))
                        s.flags |= C;
                    else if (!strcmp(value, "right") || !strcmp(value, "end"))
                        s.flags |= R;
                    else
                        valid = !strcmp(value, "left") || !strcmp(value, "start") ||
                                !strcmp(value, "justify");
                } else if (mask == 8)
                    valid = length(value, &s.indent_value, false);
                else if (mask == 16) {
                    s.hidden = !strcmp(value, "none");
                    valid = s.hidden || !strcmp(value, "block") || !strcmp(value, "inline") ||
                            !strcmp(value, "inline-block");
                } else if (mask == 32) {
                    s.pre = !strcmp(value, "pre") || !strcmp(value, "pre-wrap") ||
                            !strcmp(value, "break-spaces");
                    valid = s.pre || !strcmp(value, "normal") || !strcmp(value, "nowrap");
                } else if (mask == 64) {
                    if (!strcmp(value, "smaller"))
                        s.size_value = (rp_length_t){83, 0};
                    else if (!strcmp(value, "larger"))
                        s.size_value = (rp_length_t){120, 0};
                    else if (!strcmp(value, "small"))
                        s.size_value = (rp_length_t){80, 1};
                    else if (!strcmp(value, "medium"))
                        s.size_value = (rp_length_t){100, 1};
                    else if (!strcmp(value, "large"))
                        s.size_value = (rp_length_t){120, 1};
                    else
                        valid = length(value, &s.size_value, true);
                    valid = valid && s.size_value.value > 0;
                } else if (mask == 128) {
                    if (!strcmp(value, "normal"))
                        s.line_value = (rp_length_t){0, 0};
                    else {
                        char *tail;
                        double n = strtod(value, &tail);
                        if (tail != value && !*tail && isfinite(n) && n > 0 && n <= 10)
                            s.line_value =
                                (rp_length_t){(int16_t)(n * 100), 3}; /* inherited multiplier */
                        else
                            valid = length(value, &s.line_value, true) && s.line_value.value >= 0;
                    }
                } else if (mask == 256)
                    valid = length(value, &s.before_value, false);
                else if (mask == 512)
                    valid = length(value, &s.after_value, false);
                else if (mask == 1024) {
                    if (rp_tokens(value, "underline"))
                        s.flags |= ESPAPERPLAY_READER_BLK_UNDERLINE;
                    if (rp_tokens(value, "line-through"))
                        s.flags |= ESPAPERPLAY_READER_BLK_STRIKE;
                    valid = s.flags || !strcmp(value, "none");
                } else if (!strcmp(key, "margin")) {
                    char copy[128];
                    strcpy(copy, value);
                    char *save = NULL, *part = strtok_r(copy, " \t", &save);
                    rp_length_t values[4];
                    int count = 0;
                    while (part && count < 4 && length(part, &values[count], false)) {
                        count++;
                        part = strtok_r(NULL, " \t", &save);
                    }
                    valid = !part && count > 0;
                    if (valid) {
                        mask = 256 | 512;
                        s.before_value = values[0];
                        s.after_value = values[count > 2 ? 2 : 0];
                    }
                }
                if (valid && mask) {
                    for (unsigned bit = 1; bit <= 1024; bit <<= 1) {
                        if (!(mask & bit) || ((rule->important & bit) && !important))
                            continue;
                        apply(&rule->style, &s, bit);
                        rule->mask |= bit;
                        if (important)
                            rule->important |= bit;
                    }
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
    unsigned ranks[11] = {0};
    rp_style_t specified = {0};
    unsigned mask = 0;
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
        for (unsigned p = 0; p < 11; p++) {
            unsigned bit = 1u << p, rank = score + 1 + ((r->important & bit) ? 100000 : 0);
            if ((r->mask & bit) && rank >= ranks[p]) {
                apply(&specified, &r->style, bit);
                mask |= bit;
                ranks[p] = rank;
            }
        }
    }
    const rp_style_t initial = {.scale = 100};
    const rp_style_t *parent = depth > 1 ? &stack[depth - 2].style : &initial;
    unsigned normal = mask & ~(specified.inherit | specified.initial);
    for (unsigned bit = 1; bit <= 1024; bit <<= 1) {
        if (!(mask & bit))
            continue;
        const rp_style_t *src = specified.inherit & bit ? parent : &initial;
        if (normal & bit)
            src = &specified;
        if (bit == 1)
            style->flags = (style->flags & ~B) | (src->flags & B);
        if (bit == 2)
            style->flags = (style->flags & ~I) | (src->flags & I);
        if (bit == 4)
            style->flags = (style->flags & ~(C | R)) | (src->flags & (C | R));
        if (bit == 16)
            style->hidden = src->hidden;
        if (bit == 32)
            style->pre = src->pre;
        if (bit == 1024) {
            unsigned d = ESPAPERPLAY_READER_BLK_UNDERLINE | ESPAPERPLAY_READER_BLK_STRIKE;
            style->flags = (style->flags & ~d) | (src->flags & d);
        }
        if (!(normal & bit)) {
            if (bit == 8)
                style->indent = src->indent;
            if (bit == 64)
                style->scale = src->scale;
            if (bit == 128) {
                style->line_height = src->line_height;
                style->line_value = src->line_value;
            }
            if (bit == 256)
                style->before = src->before;
            if (bit == 512)
                style->after = src->after;
        }
    }
    if (normal & 64) {
        rp_length_t v = specified.size_value;
        int percent = v.unit == 2 ? (v.value + 10) / 20 :
                      v.unit == 1 ? v.value : (v.value * parent->scale + 50) / 100;
        style->scale = clamp(percent, 50, 200);
    }
    if (!style->scale)
        style->scale = 100;
    if (normal & 8)
        style->indent = clamp(metric(specified.indent_value, style->scale), -512, 512);
    if (normal & 128) {
        style->line_value = specified.line_value;
        style->line_height = clamp(metric(specified.line_value, style->scale), 0, 640);
    }
    if (style->line_value.unit == 3)
        style->line_height = clamp(metric(style->line_value, style->scale), 0, 640);
    if (normal & 256)
        style->before = clamp(metric(specified.before_value, style->scale), 0, 640);
    if (normal & 512)
        style->after = clamp(metric(specified.after_value, style->scale), 0, 640);
    if (mask & 256)
        style->layout |= ESPAPERPLAY_READER_LAYOUT_BEFORE;
    if (mask & 512)
        style->layout |= ESPAPERPLAY_READER_LAYOUT_AFTER;
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
