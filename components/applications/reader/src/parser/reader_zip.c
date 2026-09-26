/* SPDX-License-Identifier: MIT */
#include "reader_zip.h"
#include "zlib.h"
#include <limits.h>
#include <string.h>

#define ZIP_MAX_ENTRIES 4096
#define ZIP_MAX_NAMES (512u * 1024)
#define ZIP_MAX_ENTRY (4u * 1024 * 1024)
static uint16_t le16(const unsigned char *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }
static bool read_at(FILE *f, uint32_t off, void *p, size_t n) {
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(p, 1, n, f) == n;
}
static uint32_t name_hash(const char *p) {
    uint32_t h = 2166136261u;
    while (*p)
        h = (h ^ (unsigned char)*p++) * 16777619u;
    return h;
}
void rp_zip_close(epub_zip_t *z) {
    if (z->fp)
        fclose(z->fp);
    rp_free(z->ents);
    rp_free(z->names);
    rp_free(z->index);
    memset(z, 0, sizeof(*z));
}
bool epub_zip_ent_name(const epub_zip_t *z, int idx, char *out, size_t cap) {
    if (idx < 0 || idx >= z->ent_cnt || z->ents[idx].name_len >= cap)
        return false;
    memcpy(out, z->names + z->ents[idx].name_off, z->ents[idx].name_len + 1);
    return true;
}
int epub_zip_find(const epub_zip_t *z, const char *name) {
    if (!z->index_size)
        return -1;
    size_t p = name_hash(name) & (z->index_size - 1);
    while (z->index[p] >= 0) {
        int i = z->index[p];
        if (!strcmp(name, z->names + z->ents[i].name_off))
            return i;
        p = (p + 1) & (z->index_size - 1);
    }
    return -1;
}
esp_err_t epub_zip_parse(epub_zip_t *z) {
    if (!z || !z->fp || fseek(z->fp, 0, SEEK_END))
        return ESP_FAIL;
    long file_size = ftell(z->fp);
    if (file_size < 22 || (unsigned long)file_size > INT32_MAX)
        return ESP_ERR_INVALID_SIZE;
    z->file_size = (uint32_t)file_size;
    size_t scan = z->file_size < 65557 ? z->file_size : 65557;
    unsigned char *tail = rp_alloc(scan);
    if (!tail)
        return ESP_ERR_NO_MEM;
    if (!read_at(z->fp, z->file_size - scan, tail, scan)) {
        rp_free(tail);
        return ESP_FAIL;
    }
    int end = -1;
    for (int i = (int)scan - 22; i >= 0; --i) {
        if (le32(tail + i) == 0x06054b50 && (size_t)i + 22 + le16(tail + i + 20) == scan) {
            end = i;
            break;
        }
    }
    if (end < 0) {
        rp_free(tail);
        return ESP_ERR_INVALID_SIZE;
    }
    const unsigned char *e = tail + end;
    uint32_t cd = le32(e + 16), size = le32(e + 12), count = le16(e + 10);
    bool invalid = le16(e + 4) || le16(e + 6) || le16(e + 8) != count || count == 65535;
    uint32_t eocd = z->file_size - scan + end;
    rp_free(tail);
    if (invalid)
        return ESP_ERR_NOT_SUPPORTED;
    if (!count || count > ZIP_MAX_ENTRIES || cd > eocd || size > eocd - cd)
        return ESP_ERR_INVALID_SIZE;
    z->cd_offset = cd;
    z->ents = rp_alloc(count * sizeof(*z->ents));
    if (!z->ents)
        return ESP_ERR_NO_MEM;
    size_t names = 0;
    uint32_t p = cd;
    unsigned char h[46];
    /* First pass validates offsets and computes the exact name allocation. */
    for (uint32_t i = 0; i < count; ++i) {
        if (p > cd + size || cd + size - p < 46 || !read_at(z->fp, p, h, 46) ||
            le32(h) != 0x02014b50)
            return ESP_ERR_INVALID_SIZE;
        uint32_t n = le16(h + 28), advance = 46 + n + le16(h + 30) + le16(h + 32);
        if (!n || n >= RP_PATH || advance > cd + size - p || le16(h + 34) || le32(h + 42) >= cd)
            return ESP_ERR_INVALID_SIZE;
        names += n + 1;
        if (names > ZIP_MAX_NAMES)
            return ESP_ERR_INVALID_SIZE;
        z->ents[i] = (epub_zip_ent_t){.lfh_off = le32(h + 42),
                                      .csize = le32(h + 20),
                                      .usize = le32(h + 24),
                                      .crc = le32(h + 16),
                                      .method = le16(h + 10),
                                      .flags = le16(h + 8),
                                      .name_len = n};
        if (z->ents[i].csize == UINT32_MAX || z->ents[i].usize == UINT32_MAX)
            return ESP_ERR_NOT_SUPPORTED;
        p += advance;
    }
    z->names = rp_alloc(names);
    if (!z->names)
        return ESP_ERR_NO_MEM;
    z->index_size = 1;
    while (z->index_size < count * 2)
        z->index_size *= 2;
    z->index = rp_alloc(z->index_size * sizeof(int));
    if (!z->index)
        return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < z->index_size; i++)
        z->index[i] = -1;
    p = cd;
    size_t np = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!read_at(z->fp, p, h, 46))
            return ESP_FAIL;
        epub_zip_ent_t *ent = &z->ents[i];
        ent->name_off = np;
        if (fread(z->names + np, 1, ent->name_len, z->fp) != ent->name_len)
            return ESP_FAIL;
        if (memchr(z->names + np, 0, ent->name_len))
            return ESP_ERR_INVALID_ARG;
        z->names[np + ent->name_len] = 0;
        size_t slot = name_hash(z->names + np) & (z->index_size - 1);
        while (z->index[slot] >= 0) {
            if (!strcmp(z->names + np, z->names + z->ents[z->index[slot]].name_off))
                return ESP_ERR_INVALID_ARG;
            slot = (slot + 1) & (z->index_size - 1);
        }
        z->index[slot] = i;
        np += ent->name_len + 1;
        p += 46 + ent->name_len + le16(h + 30) + le16(h + 32);
    }
    z->ent_cnt = count;
    z->names_len = np;
    return ESP_OK;
}
static voidpf zip_alloc(voidpf opaque, uInt n, uInt size) {
    (void)opaque;
    if (size && n > SIZE_MAX / size)
        return NULL;
    return rp_alloc((size_t)n * size);
}
static void zip_free(voidpf opaque, voidpf p) {
    (void)opaque;
    rp_free(p);
}
esp_err_t epub_zip_extract_unlocked(epub_zip_t *z, int idx, char **out, size_t *len) {
    if (!out)
        return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (len)
        *len = 0;
    if (idx < 0 || idx >= z->ent_cnt)
        return ESP_ERR_NOT_FOUND;
    const epub_zip_ent_t *e = &z->ents[idx];
    if (e->flags & 0x2041 || (e->method != 0 && e->method != 8))
        return ESP_ERR_NOT_SUPPORTED;
    if (e->usize > ZIP_MAX_ENTRY)
        return ESP_ERR_INVALID_SIZE;
    unsigned char h[30];
    if (!read_at(z->fp, e->lfh_off, h, 30) || le32(h) != 0x04034b50 || le16(h + 8) != e->method ||
        le16(h + 6) != e->flags)
        return ESP_ERR_INVALID_SIZE;
    uint64_t data = (uint64_t)e->lfh_off + 30 + le16(h + 26) + le16(h + 28);
    if (data > z->cd_offset || e->csize > z->cd_offset - data || le16(h + 26) != e->name_len)
        return ESP_ERR_INVALID_SIZE;
    char name[RP_PATH];
    if (fread(name, 1, e->name_len, z->fp) != e->name_len ||
        memcmp(name, z->names + e->name_off, e->name_len))
        return ESP_ERR_INVALID_SIZE;
    if (fseek(z->fp, (long)data, SEEK_SET))
        return ESP_FAIL;
    char *buf = rp_alloc((size_t)e->usize + 1);
    if (!buf)
        return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_FAIL;
    if (e->method == 0) {
        if (e->csize == e->usize && fread(buf, 1, e->usize, z->fp) == e->usize)
            err = ESP_OK;
    } else {
        unsigned char *input = rp_alloc(8192);
        if (!input) {
            rp_free(buf);
            return ESP_ERR_NO_MEM;
        }
        z_stream stream = {.zalloc = zip_alloc, .zfree = zip_free};
        if (inflateInit2(&stream, -MAX_WBITS) == Z_OK) {
            stream.next_out = (Bytef *)buf;
            stream.avail_out = e->usize + 1;
            uint32_t left = e->csize;
            int status = Z_OK;
            while (status == Z_OK) {
                if (!stream.avail_in && left) {
                    size_t n = left < 8192 ? left : 8192;
                    if (fread(input, 1, n, z->fp) != n)
                        break;
                    left -= n;
                    stream.next_in = input;
                    stream.avail_in = n;
                }
                uLong before_in = stream.total_in, before_out = stream.total_out;
                status = inflate(&stream, Z_NO_FLUSH);
                if (status == Z_STREAM_END) {
                    if (stream.total_out == e->usize && stream.total_in == e->csize)
                        err = ESP_OK;
                    break;
                }
                if (!stream.avail_out ||
                    (stream.total_in == before_in && stream.total_out == before_out))
                    break;
            }
            inflateEnd(&stream);
        }
        rp_free(input);
    }
    if (err == ESP_OK && (uint32_t)crc32(0, (Bytef *)buf, e->usize) != e->crc)
        err = ESP_ERR_INVALID_CRC;
    if (err != ESP_OK) {
        rp_free(buf);
        return err;
    }
    buf[e->usize] = 0;
    *out = buf;
    if (len)
        *len = e->usize;
    return ESP_OK;
}
esp_err_t rp_zip_xml(epub_zip_t *z, int idx, char **out, size_t *len) {
    *out = NULL;
    if (idx < 0 || idx >= z->ent_cnt)
        return ESP_ERR_NOT_FOUND;
    if (z->ents[idx].usize > RP_MAX_TEXT)
        return ESP_ERR_INVALID_SIZE;
    size_t n = 0;
    esp_err_t err = epub_zip_extract_unlocked(z, idx, out, &n);
    if (err == ESP_OK)
        err = rp_xml_utf8(out, &n);
    if (err != ESP_OK) {
        rp_free(*out);
        *out = NULL;
    }
    if (len)
        *len = n;
    return err;
}
