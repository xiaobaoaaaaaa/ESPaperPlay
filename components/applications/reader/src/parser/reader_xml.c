/* SPDX-License-Identifier: MIT */
#include "reader_parse.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>

size_t rp_utf8_encode(uint32_t c, char out[4]) {
    if (!c || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        c = 0xfffd;
    if (c < 0x80) {
        out[0] = c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = 0xc0 | (c >> 6);
        out[1] = 0x80 | (c & 63);
        return 2;
    }
    if (c < 0x10000) {
        out[0] = 0xe0 | (c >> 12);
        out[1] = 0x80 | ((c >> 6) & 63);
        out[2] = 0x80 | (c & 63);
        return 3;
    }
    out[0] = 0xf0 | (c >> 18);
    out[1] = 0x80 | ((c >> 12) & 63);
    out[2] = 0x80 | ((c >> 6) & 63);
    out[3] = 0x80 | (c & 63);
    return 4;
}
uint32_t rp_utf8_next(const char *s, size_t n, size_t *used) {
    *used = n ? 1 : 0;
    if (!n)
        return 0xfffd;
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80)
        return p[0] ? p[0] : 0xfffd;
    int bytes = p[0] >= 0xc2 && p[0] <= 0xdf   ? 2
                : p[0] >= 0xe0 && p[0] <= 0xef ? 3
                : p[0] >= 0xf0 && p[0] <= 0xf4 ? 4
                                               : 0;
    if (!bytes || n < (size_t)bytes)
        return 0xfffd;
    uint32_t c = p[0] & ((1u << (7 - bytes)) - 1);
    for (int i = 1; i < bytes; i++) {
        if ((p[i] & 0xc0) != 0x80)
            return 0xfffd;
        c = (c << 6) | (p[i] & 63);
    }
    if ((bytes == 3 && c < 0x800) || (bytes == 4 && c < 0x10000) || c > 0x10ffff ||
        (c >= 0xd800 && c <= 0xdfff))
        return 0xfffd;
    *used = bytes;
    return c;
}
/* XML predefined, numeric, and common HTML publishing entities. Unknown entities
 * remain literal; external DTDs and custom entity declarations are never loaded. */
static uint32_t entity(const char *s, size_t n) {
    static const struct {
        const char *name;
        uint32_t cp;
    } named[] = {{"amp", 38},      {"lt", 60},       {"gt", 62},      {"quot", 34},
                 {"apos", 39},     {"nbsp", 160},    {"ensp", 8194},  {"emsp", 8195},
                 {"thinsp", 8201}, {"zwnj", 8204},   {"zwj", 8205},   {"ndash", 8211},
                 {"mdash", 8212},  {"lsquo", 8216},  {"rsquo", 8217}, {"ldquo", 8220},
                 {"rdquo", 8221},  {"hellip", 8230}, {"bull", 8226},  {"copy", 169},
                 {"reg", 174},     {"trade", 8482},  {"laquo", 171},  {"raquo", 187},
                 {"shy", 173},     {"middot", 183},  {"times", 215},  {"divide", 247},
                 {"euro", 8364},   {"pound", 163},   {"yen", 165}};
    if (n > 1 && s[0] == '#') {
        size_t i = 1;
        unsigned base = 10;
        uint32_t v = 0;
        if (i < n && (s[i] == 'x' || s[i] == 'X')) {
            base = 16;
            i++;
        }
        if (i == n)
            return 0;
        for (; i < n; i++) {
            int d = s[i] >= '0' && s[i] <= '9'   ? s[i] - '0'
                    : s[i] >= 'a' && s[i] <= 'f' ? s[i] - 'a' + 10
                    : s[i] >= 'A' && s[i] <= 'F' ? s[i] - 'A' + 10
                                                 : -1;
            if (d < 0 || (unsigned)d >= base || v > 0x10ffff / base)
                return 0xfffd;
            v = v * base + d;
        }
        return !v || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff) ? 0xfffd : v;
    }
    for (size_t i = 0; i < sizeof(named) / sizeof(*named); i++)
        if (strlen(named[i].name) == n && !memcmp(s, named[i].name, n))
            return named[i].cp;
    return 0;
}
size_t rp_entities(const char *s, size_t n, char *out, size_t cap) {
    size_t r = 0, w = 0;
    while (r < n) {
        size_t used = 0;
        uint32_t c = 0;
        if (s[r] == '&') {
            size_t e = r + 1;
            while (e < n && e - r < 32 && s[e] != ';' && !isspace((unsigned char)s[e]))
                e++;
            if (e < n && s[e] == ';') {
                c = entity(s + r + 1, e - r - 1);
                if (c)
                    used = e - r + 1;
            }
        }
        if (!used)
            c = rp_utf8_next(s + r, n - r, &used);
        char u[4];
        size_t k = rp_utf8_encode(c, u);
        if (w + k >= cap)
            break;
        memcpy(out + w, u, k);
        w += k;
        r += used;
    }
    if (cap)
        out[w] = 0;
    return w;
}
bool rp_tokens(const char *list, const char *token) {
    size_t n = strlen(token);
    while (*list) {
        while (isspace((unsigned char)*list))
            list++;
        const char *p = list;
        while (*list && !isspace((unsigned char)*list))
            list++;
        if ((size_t)(list - p) == n && !memcmp(p, token, n))
            return true;
    }
    return false;
}
static int hex(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : -1;
}
static bool percent(const char *s, size_t n, char *out, size_t cap) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '%') {
            if (i + 2 >= n || hex(s[i + 1]) < 0 || hex(s[i + 2]) < 0)
                return false;
            c = (hex(s[i + 1]) << 4) | hex(s[i + 2]);
            i += 2;
        }
        if (!c || c == '\\' || c < 32 || w + 1 >= cap)
            return false;
        out[w++] = c;
    }
    out[w] = 0;
    return true;
}
bool rp_uri(const char *doc, const char *ref, char out[RP_PATH], char frag[128]) {
    char path[RP_PATH], joined[RP_PATH];
    if (frag)
        frag[0] = 0;
    const char *hash = strchr(ref, '#');
    size_t n = hash ? (size_t)(hash - ref) : strlen(ref);
    const char *q = memchr(ref, '?', n);
    if (q)
        n = q - ref;
    if (hash && frag && !percent(hash + 1, strlen(hash + 1), frag, 128))
        return false;
    if (!percent(ref, n, path, sizeof(path)))
        return false;
    if (strchr(path, ':') || !strncmp(path, "//", 2))
        return false;
    if (!path[0]) {
        if (strlen(doc) >= sizeof(joined))
            return false;
        strcpy(joined, doc);
    } else if (path[0] == '/') {
        if (strlen(path) >= sizeof(joined))
            return false;
        strcpy(joined, path + 1);
    } else {
        const char *slash = strrchr(doc, '/');
        size_t base = slash ? (size_t)(slash - doc) + 1 : 0;
        if (base + strlen(path) >= sizeof(joined))
            return false;
        memcpy(joined, doc, base);
        strcpy(joined + base, path);
    }
    size_t w = 0;
    const char *p = joined;
    while (*p) {
        const char *e = strchr(p, '/');
        if (!e)
            e = p + strlen(p);
        size_t k = e - p;
        if (k == 2 && !memcmp(p, "..", 2)) {
            if (!w)
                return false;
            while (w && out[w - 1] != '/')
                w--;
            if (w)
                w--;
        } else if (k && !(k == 1 && *p == '.')) {
            if (w)
                out[w++] = '/';
            if (w + k >= RP_PATH)
                return false;
            memcpy(out + w, p, k);
            w += k;
        }
        p = *e ? e + 1 : e;
    }
    out[w] = 0;
    return w > 0;
}
void rp_xml_init(rp_xml_t *x, const char *data, size_t len) {
    x->pos = data;
    x->end = data + len;
}
static const char *bounded(const char *p, const char *end, const char *needle) {
    size_t n = strlen(needle);
    for (; (size_t)(end - p) >= n; p++)
        if (!memcmp(p, needle, n))
            return p;
    return NULL;
}
rp_xml_kind_t rp_xml_next(rp_xml_t *x, rp_xml_token_t *t) {
    memset(t, 0, sizeof(*t));
    for (;;) {
        const char *p = x->pos;
        if (p >= x->end)
            return t->kind = RP_XML_EOF;
        t->raw = p;
        if (*p != '<') {
            const char *e = memchr(p, '<', x->end - p);
            if (!e)
                e = x->end;
            t->text = p;
            t->text_len = e - p;
            t->end = x->pos = e;
            return t->kind = RP_XML_TEXT;
        }
        if (x->end - p >= 4 && !memcmp(p, "<!--", 4)) {
            const char *e = bounded(p + 4, x->end, "-->");
            if (!e)
                goto bad;
            x->pos = e + 3;
            continue;
        }
        if (x->end - p >= 9 && !memcmp(p, "<![CDATA[", 9)) {
            const char *e = bounded(p + 9, x->end, "]]>");
            if (!e)
                goto bad;
            t->text = p + 9;
            t->text_len = e - p - 9;
            t->cdata = true;
            t->end = x->pos = e + 3;
            return t->kind = RP_XML_TEXT;
        }
        if (x->end - p >= 2 && (p[1] == '!' || p[1] == '?')) {
            char quote = 0;
            int brackets = 0;
            const char *e = p + 2;
            for (; e < x->end; e++) {
                if (quote) {
                    if (*e == quote)
                        quote = 0;
                } else if (*e == '\'' || *e == '"')
                    quote = *e;
                else if (*e == '[')
                    brackets++;
                else if (*e == ']')
                    brackets--;
                else if (*e == '>' && brackets <= 0)
                    break;
            }
            if (e == x->end)
                goto bad;
            x->pos = e + 1;
            continue;
        }
        p++;
        bool close = p < x->end && *p == '/';
        if (close)
            p++;
        const char *name = p;
        while (p < x->end &&
               (isalnum((unsigned char)*p) || *p == ':' || *p == '_' || *p == '-' || *p == '.'))
            p++;
        if (p == name)
            goto bad;
        const char *local = name;
        for (const char *q = name; q < p; q++)
            if (*q == ':')
                local = q + 1;
        size_t n = p - local;
        if (n >= sizeof(t->name))
            goto bad;
        for (size_t i = 0; i < n; i++)
            t->name[i] = tolower((unsigned char)local[i]);
        char quote = 0;
        const char *e = p;
        for (; e < x->end; e++) {
            if (quote) {
                if (*e == quote)
                    quote = 0;
            } else if (*e == '\'' || *e == '"')
                quote = *e;
            else if (*e == '>')
                break;
        }
        if (e == x->end)
            goto bad;
        const char *last = e;
        while (last > p && isspace((unsigned char)last[-1]))
            last--;
        t->empty = !close && last > p && last[-1] == '/';
        t->text = p;
        t->end = e;
        t->text_len = e - p;
        x->pos = e + 1;
        return t->kind = close ? RP_XML_END : RP_XML_START;
    }
bad:
    x->pos = x->end;
    return t->kind = RP_XML_ERROR;
}
bool rp_xml_attr(const rp_xml_token_t *t, const char *name, char *out, size_t cap) {
    if (!cap)
        return false;
    out[0] = 0;
    const char *p = t->text, *end = t->end;
    size_t want = strlen(name);
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == '/'))
            p++;
        const char *a = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '=' && *p != '/' && *p != '>')
            p++;
        size_t n = p - a;
        if (!n) {
            p++;
            continue;
        }
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (p == end || *p != '=')
            continue;
        p++;
        while (p < end && isspace((unsigned char)*p))
            p++;
        char quote = p < end && (*p == '\'' || *p == '"') ? *p++ : 0;
        const char *v = p;
        if (quote)
            while (p < end && *p != quote)
                p++;
        else
            while (p < end && !isspace((unsigned char)*p) && *p != '>')
                p++;
        size_t k = p - v;
        if (quote && p < end)
            p++;
        bool match = n == want && !strncasecmp(a, name, want);
        if (!strchr(name, ':')) {
            const char *colon = memchr(a, ':', n);
            if (colon)
                match = (size_t)(a + n - colon - 1) == want && !strncasecmp(colon + 1, name, want);
        }
        if (match) {
            if (k >= cap)
                return false;
            rp_entities(v, k, out, cap);
            return true;
        }
    }
    return false;
}
esp_err_t rp_xml_utf8(char **data, size_t *len) {
    unsigned char *s = (unsigned char *)*data;
    size_t n = *len;
    if (n >= 4 && (!memcmp(s, "\xff\xfe\0\0", 4) || !memcmp(s, "\0\0\xfe\xff", 4)))
        return ESP_ERR_NOT_SUPPORTED;
    if (n >= 2 && ((s[0] == 0xff && s[1] == 0xfe) || (s[0] == 0xfe && s[1] == 0xff))) {
        if (n % 2 || n > RP_MAX_TEXT)
            return ESP_ERR_INVALID_SIZE;
        bool le = s[0] == 0xff;
        size_t cap = n * 2 < RP_MAX_TEXT ? n * 2 : RP_MAX_TEXT;
        char *out = rp_alloc(cap + 1);
        if (!out)
            return ESP_ERR_NO_MEM;
        size_t w = 0;
        for (size_t i = 2; i < n; i += 2) {
            uint32_t c = le ? s[i] | s[i + 1] << 8 : s[i] << 8 | s[i + 1];
            if (c >= 0xd800 && c <= 0xdbff && i + 3 < n) {
                uint32_t d = le ? s[i + 2] | s[i + 3] << 8 : s[i + 2] << 8 | s[i + 3];
                if (d >= 0xdc00 && d <= 0xdfff) {
                    c = 0x10000 + ((c - 0xd800) << 10) + d - 0xdc00;
                    i += 2;
                }
            }
            char u[4];
            size_t k = rp_utf8_encode(c, u);
            if (w + k > cap) {
                rp_free(out);
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(out + w, u, k);
            w += k;
        }
        out[w] = 0;
        rp_free(*data);
        *data = out;
        *len = w;
        return ESP_OK;
    }
    if (memchr(s, 0, n))
        return ESP_ERR_INVALID_ARG;
    if (n >= 3 && !memcmp(s, "\xef\xbb\xbf", 3)) {
        memmove(s, s + 3, n - 3);
        n -= 3;
        s[n] = 0;
        *len = n;
    }
    return ESP_OK;
}
