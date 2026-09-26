/* SPDX-License-Identifier: MIT */
#include "reader_image.h"
#include "esp_log.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "zlib.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static const char *TAG = "Reader_IMAGE";
static uint8_t png_subbyte(const uint8_t *px, size_t bit_idx, uint8_t bd) {
    const uint8_t byte = px[bit_idx >> 3];
    const unsigned sh = (unsigned)(8 - bd - (bit_idx & 7));
    return (uint8_t)((byte >> sh) & ((1u << bd) - 1u));
}

/**
 * PNG 单行处理：先按过滤类型反演（none/sub/up/average/paeth），再对采样列
 * 提取像素转 RGB565。prev 为上一行原始缓冲（NULL=首行，按 0 处理）。
 */
static bool png_handle_row(uint8_t *cur, const uint8_t *prev, uint32_t w, uint8_t bd, uint8_t ct,
                           const uint8_t *plte, size_t plte_len, const uint8_t *trns,
                           size_t trns_len, int step, int dw, uint16_t *dst, uint32_t row, int dh) {
    static const uint8_t ch[] = {1, 0, 3, 1, 2, 0, 4}; /* 颜色类型 → 通道数 */
    const size_t bpp_bits = (size_t)ch[ct] * bd;
    const size_t row_bytes = (((size_t)w * bpp_bits) + 7) / 8;
    const size_t fu = (bpp_bits + 7) / 8; /* 过滤单元 */
    uint8_t *px = &cur[1];
    const uint8_t ftype = cur[0];

    if (ftype != 0) { /* 0=none：原样，无需反演 */
        for (size_t i = 0; i < row_bytes; i++) {
            const int a = (i >= fu) ? px[i - fu] : 0;
            const int b = (prev != NULL) ? prev[1 + i] : 0;
            const int c = (i >= fu && prev != NULL) ? prev[1 + i - fu] : 0;
            int v = px[i];
            switch (ftype) {
            case 1:
                v += a;
                break;
            case 2:
                v += b;
                break;
            case 3:
                v += (a + b) / 2;
                break;
            case 4: {
                const int pp = a + b - c;
                const int pa = pp > a ? pp - a : a - pp;
                const int pb = pp > b ? pp - b : b - pp;
                const int pc = pp > c ? pp - c : c - pp;
                v += (pa <= pb && pa <= pc) ? a : ((pb <= pc) ? b : c);
                break;
            }
            default:
                return false;
            }
            px[i] = (uint8_t)v;
        }
    }

    if (row % (uint32_t)step != 0) {
        return true;
    }
    const uint32_t dy = row / (uint32_t)step;
    if ((int)dy >= dh) {
        return true;
    }
    uint16_t *drow = &dst[(size_t)dy * dw];
    for (int x = 0; x < dw; x++) {
        const uint32_t sx = (uint32_t)x * (uint32_t)step;
        uint8_t r = 0, g = 0, b = 0, a = 255;
        if (ct == 2 || ct == 4 || ct == 6) {
            const size_t off = (size_t)sx * ch[ct] * (bd / 8);
            if (bd == 16) { /* 16 位深取高字节（大端） */
                if (ct == 4) {
                    r = g = b = px[off];
                    a = px[off + 2];
                } else {
                    r = px[off];
                    g = px[off + 2];
                    b = px[off + 4];
                    a = (ct == 6) ? px[off + 6] : 255;
                }
            } else {
                if (ct == 4) {
                    r = g = b = px[off];
                    a = px[off + 1];
                } else {
                    r = px[off];
                    g = px[off + 1];
                    b = px[off + 2];
                    a = (ct == 6) ? px[off + 3] : 255;
                }
            }
        } else if (ct == 0) {
            uint8_t gray;
            if (bd == 16) {
                gray = px[(size_t)sx * 2];
            } else if (bd == 8) {
                gray = px[sx];
            } else {
                gray = png_subbyte(px, (size_t)sx * bd, bd);
                gray = (uint8_t)(gray * 255 / ((1u << bd) - 1u)); /* 拉伸到 0-255 */
            }
            r = g = b = gray;
        } else { /* ct == 3：调色板 */
            const uint8_t idx = (bd == 8) ? px[sx] : png_subbyte(px, (size_t)sx * bd, bd);
            if ((size_t)idx * 3 + 2 < plte_len) {
                r = plte[idx * 3];
                g = plte[idx * 3 + 1];
                b = plte[idx * 3 + 2];
            } else
                return false;
            if (trns_len > idx)
                a = trns[idx];
        }
        if (ct == 0 && trns_len == 2) {
            uint16_t sample = bd == 16  ? (px[sx * 2] << 8) | px[sx * 2 + 1]
                              : bd == 8 ? px[sx]
                                        : png_subbyte(px, (size_t)sx * bd, bd);
            if (sample == ((uint16_t)trns[0] << 8 | trns[1]))
                a = 0;
        }
        if (ct == 2 && trns_len == 6) {
            size_t off = (size_t)sx * 3 * (bd / 8);
            bool transparent = true;
            for (int channel = 0; channel < 3; channel++) {
                uint16_t sample = bd == 16
                                      ? (px[off + channel * 2] << 8) | px[off + channel * 2 + 1]
                                      : px[off + channel];
                if (sample != ((uint16_t)trns[channel * 2] << 8 | trns[channel * 2 + 1]))
                    transparent = false;
            }
            if (transparent)
                a = 0;
        }
        r = (r * a + 255 * (255 - a) + 127) / 255;
        g = (g * a + 255 * (255 - a) + 127) / 255;
        b = (b * a + 255 * (255 - a) + 127) / 255;
        drow[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    return true;
}

/** 图片解码输出（RGB565，缓冲 PSRAM 堆分配，调用方释放）。 */

/** PNG：zlib 流式逐行解码 + unfilter + 行内最近邻抽样（峰值内存 ≈ 2 行扫描线）。 */
static esp_err_t epub_decode_png(const uint8_t *src, size_t src_len, int max_w, int max_h,
                                 epub_img_out_t *out) {
    /* 文件签名 + IHDR */
    if (src_len < 33 || memcmp(src, "\x89PNG\r\n\x1a\n", 8) || memcmp(&src[12], "IHDR", 4) != 0 ||
        src[8] || src[9] || src[10] || src[11] != 13 || src[26] || src[27]) {
        return ESP_FAIL;
    }
    const uint32_t w =
        ((uint32_t)src[16] << 24) | ((uint32_t)src[17] << 16) | ((uint32_t)src[18] << 8) | src[19];
    const uint32_t h =
        ((uint32_t)src[20] << 24) | ((uint32_t)src[21] << 16) | ((uint32_t)src[22] << 8) | src[23];
    const uint8_t bd = src[24];    /* 位深 */
    const uint8_t ct = src[25];    /* 颜色类型 */
    const uint8_t ilace = src[28]; /* 隔行 */
    if (w == 0 || h == 0 || w > 8000 || h > 8000 || ilace != 0) {
        ESP_LOGW(TAG, "epub: png unsupported (%ux%u bd=%u ct=%u ilace=%u)", (unsigned)w,
                 (unsigned)h, bd, ct, ilace);
        return ESP_ERR_NOT_SUPPORTED;
    }
    bool ok_depth = false;
    if (ct == 0) {
        ok_depth = (bd == 1 || bd == 2 || bd == 4 || bd == 8 || bd == 16);
    } else if (ct == 2 || ct == 4 || ct == 6) {
        ok_depth = (bd == 8 || bd == 16);
    } else if (ct == 3) {
        ok_depth = (bd == 1 || bd == 2 || bd == 4 || bd == 8);
    }
    if (!ok_depth) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    static const uint8_t ch[] = {1, 0, 3, 1, 2, 0, 4};
    const size_t bits_pp = (size_t)ch[ct] * bd;
    const size_t row_bytes = ((size_t)w * bits_pp + 7) / 8;
    const size_t raw_row = 1 + row_bytes; /* 过滤字节 + 像素数据 */

    /* 抽样步长（任意整数，非 2 的幂，质量更好） */
    int step = 1;
    while (((w + (uint32_t)step - 1) / (uint32_t)step) > (uint32_t)max_w ||
           ((h + (uint32_t)step - 1) / (uint32_t)step) > (uint32_t)max_h ||
           ((uint64_t)w + step - 1) / step * (((uint64_t)h + step - 1) / step) > 1024u * 1024) {
        step++;
    }
    const int dw = (int)((w + (uint32_t)step - 1) / (uint32_t)step);
    const int dh = (int)((h + (uint32_t)step - 1) / (uint32_t)step);

    uint8_t plte[768];
    size_t plte_len = 0;
    uint8_t trns[256];
    size_t trns_len = 0;

    uint8_t *rows = heap_caps_calloc(1, raw_row * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *dst = heap_caps_malloc((size_t)dw * dh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rows == NULL || dst == NULL) {
        heap_caps_free(rows);
        heap_caps_free(dst);
        return ESP_ERR_NO_MEM;
    }
    memset(dst, 0xFF, (size_t)dw * dh * 2);

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    const bool zinit = inflateInit(&zs) == Z_OK; /* PNG IDAT 为 zlib 包装流 */
    bool failed = !zinit, ended = false, have_end = false;
    size_t row_filled = 0;
    uint32_t row = 0;

    size_t p = 8; /* 跳过签名 */
    while (!failed && p + 8 <= src_len) {
        const uint32_t clen = ((uint32_t)src[p] << 24) | ((uint32_t)src[p + 1] << 16) |
                              ((uint32_t)src[p + 2] << 8) | src[p + 3];
        const char *ctype = (const char *)&src[p + 4];
        const size_t cdata = p + 8;
        if (src_len - cdata < 4 || clen > src_len - cdata - 4) {
            failed = true;
            break;
        }
        const uint8_t *crc = src + cdata + clen;
        uint32_t expected =
            (uint32_t)crc[0] << 24 | (uint32_t)crc[1] << 16 | (uint32_t)crc[2] << 8 | crc[3];
        if ((uint32_t)crc32(0, src + p + 4, clen + 4) != expected) {
            failed = true;
            break;
        }
        if (memcmp(ctype, "IDAT", 4) == 0) {
            if (ended && clen) {
                failed = true;
                break;
            }
            zs.next_in = (Bytef *)&src[cdata];
            zs.avail_in = (uInt)clen;
            while (!failed && zs.avail_in > 0) {
                uint8_t *cur = &rows[(row % 2) * raw_row];
                uint8_t overflow;
                bool pixels_done = row >= h;
                zs.next_out = pixels_done ? &overflow : &cur[row_filled];
                zs.avail_out = pixels_done ? 1 : (uInt)(raw_row - row_filled);
                uLong before_in = zs.total_in, before_out = zs.total_out;
                const int zr = inflate(&zs, Z_NO_FLUSH);
                if (pixels_done && zs.total_out != before_out) {
                    failed = true;
                    break;
                }
                if (!pixels_done)
                    row_filled = raw_row - zs.avail_out;
                if (zr == Z_STREAM_END)
                    ended = true;
                if (zr != Z_STREAM_END && zs.total_in == before_in && zs.total_out == before_out) {
                    failed = true;
                    break;
                }
                if (zr != Z_OK && zr != Z_BUF_ERROR && zr != Z_STREAM_END) {
                    failed = true;
                    break;
                }
                if (row_filled >= raw_row) {
                    const uint8_t *prev = (row > 0) ? &rows[((row + 1) % 2) * raw_row] : NULL;
                    if (!png_handle_row(cur, prev, w, bd, ct, plte, plte_len, trns, trns_len, step,
                                        dw, dst, row, dh)) {
                        failed = true;
                        break;
                    }
                    row++;
                    row_filled = 0;
                }
                if (zr == Z_STREAM_END) {
                    if (zs.avail_in)
                        failed = true;
                    break;
                }
            }
        } else if (memcmp(ctype, "PLTE", 4) == 0) {
            plte_len = clen <= sizeof(plte) ? clen : 0;
            if (plte_len > 0) {
                memcpy(plte, &src[cdata], plte_len);
            }
        } else if (memcmp(ctype, "tRNS", 4) == 0) {
            trns_len = clen <= sizeof(trns) ? clen : 0;
            if (trns_len > 0) {
                memcpy(trns, &src[cdata], trns_len);
            }
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            have_end = clen == 0;
            break;
        }
        p = cdata + clen + 4; /* 数据 + CRC */
    }
    if (zinit) {
        inflateEnd(&zs);
    }
    heap_caps_free(rows);
    if (failed || row != h || row_filled || !ended || !have_end) {
        ESP_LOGW(TAG, "epub: png decode failed (row %u/%u)", (unsigned)row, (unsigned)h);
        heap_caps_free(dst);
        return ESP_FAIL;
    }
    out->buf = (uint8_t *)dst;
    out->w = dw;
    out->h = dh;
    return ESP_OK;
}

/* TJpgD 的 jd.device 同时作为输入与输出回调的上下文（jd_prepare 传入后固定），
 * 因此输入源与输出目标必须合并在同一个结构里。 */
typedef struct {
    /* 输入（jpg_in_func） */
    const uint8_t *src;
    size_t src_len;
    size_t pos;
    /* 输出（jpg_out_func） */
    uint16_t *dst; /* 目标 RGB565 */
    int dw, dh;    /* 目标尺寸 */
    int step;      /* 抽样步长 */
} jpg_ctx_t;

static size_t jpg_in_func(JDEC *jd, uint8_t *buf, size_t nbyte) {
    jpg_ctx_t *c = (jpg_ctx_t *)jd->device;
    size_t avail = c->src_len - c->pos;
    if (nbyte > avail) {
        nbyte = avail;
    }
    if (buf != NULL) {
        if (nbyte > 0) {
            memcpy(buf, &c->src[c->pos], nbyte);
        }
    }
    c->pos += nbyte;
    return nbyte;
}

static int jpg_out_func(JDEC *jd, void *bitmap, JRECT *rect) {
    jpg_ctx_t *o = (jpg_ctx_t *)jd->device;
    const uint8_t *px = (const uint8_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        if (y % o->step != 0 || y / o->step >= o->dh) {
            continue;
        }
        const int dy = y / o->step;
        uint16_t *drow = &o->dst[(size_t)dy * o->dw];
        const uint8_t *srow = &px[(size_t)(y - rect->top) * (rect->right - rect->left + 1) * 3];
        for (int x = rect->left; x <= rect->right; x++) {
            if (x % o->step != 0 || x / o->step >= o->dw) {
                continue;
            }
            const uint8_t *c = &srow[(size_t)(x - rect->left) * 3];
            const uint16_t r = c[0] >> 3, g = c[1] >> 2, b = c[2] >> 3;
            drow[x / o->step] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    return 1; /* 继续解码 */
}

/** JPEG：TJpgD 逐 MCU 解码 + 解码回调内最近邻抽样（峰值内存 = 目标缓冲）。 */
static esp_err_t epub_decode_jpeg(const uint8_t *src, size_t src_len, int max_w, int max_h,
                                  epub_img_out_t *out) {
    uint8_t *work = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (work == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* TJpgD 建议 3100 字节工作区，富余取 4KB */
    jpg_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.src = src;
    ctx.src_len = src_len;
    JDEC jd;
    memset(&jd, 0, sizeof(jd));
    JRESULT r = jd_prepare(&jd, jpg_in_func, work, 4096, &ctx);
    if (r != JDR_OK) {
        heap_caps_free(work);
        ESP_LOGW(TAG, "epub: jd_prepare failed (%d)", r);
        return ESP_FAIL;
    }
    int sw = jd.width, sh = jd.height;
    if (sw <= 0 || sh <= 0 || sw > 16000 || sh > 16000) {
        heap_caps_free(work);
        return ESP_FAIL;
    }
    int step = 1;
    while ((sw + step - 1) / step > max_w || (sh + step - 1) / step > max_h ||
           ((uint64_t)sw + step - 1) / step * (((uint64_t)sh + step - 1) / step) > 1024u * 1024) {
        step++;
    }
    const int dw = (sw + step - 1) / step;
    const int dh = (sh + step - 1) / step;
    ctx.dw = dw;
    ctx.dh = dh;
    ctx.step = step;
    ctx.dst = heap_caps_malloc((size_t)dw * dh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx.dst == NULL) {
        heap_caps_free(work);
        return ESP_ERR_NO_MEM;
    }
    memset(ctx.dst, 0xFF, (size_t)dw * dh * 2);
    r = jd_decomp(&jd, jpg_out_func, 0);
    heap_caps_free(work);
    if (r != JDR_OK) {
        heap_caps_free(ctx.dst);
        ESP_LOGW(TAG, "epub: jd_decomp failed (%d)", r);
        return ESP_FAIL;
    }
    out->buf = (uint8_t *)ctx.dst;
    out->w = dw;
    out->h = dh;
    return ESP_OK;
}

/** 按扩展名 / 魔数分派 JPEG / PNG 解码（封面探测与章节图片共用）。 */
static esp_err_t decode(const char *name, const uint8_t *data, size_t len, int max_w, int max_h,
                        epub_img_out_t *out) {
    const char *dot = strrchr(name, '.');
    if (dot != NULL && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)) {
        return epub_decode_jpeg(data, len, max_w, max_h, out);
    }
    if (dot != NULL && strcasecmp(dot, ".png") == 0) {
        return epub_decode_png(data, len, max_w, max_h, out);
    }
    if (len > 3 && (uint8_t)data[0] == 0xFF && (uint8_t)data[1] == 0xD8) {
        return epub_decode_jpeg(data, len, max_w, max_h, out);
    }
    if (len > 8 && (uint8_t)data[1] == 'P' && (uint8_t)data[2] == 'N' && (uint8_t)data[3] == 'G') {
        return epub_decode_png(data, len, max_w, max_h, out);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rp_image_decode(const char *name, const uint8_t *data, size_t len, int max_w, int max_h,
                          epub_img_out_t *out) {
    if (!name || !data || !out || max_w <= 0 || max_h <= 0)
        return ESP_ERR_INVALID_ARG;
    /* The view requests 2x sampling for grayscale. Accept that budget while
     * bounding the RGB565 allocation to 2 MiB, independently of source size. */
    if (max_w > 1600)
        max_w = 1600;
    if (max_h > 1600)
        max_h = 1600;
    memset(out, 0, sizeof(*out));
    return decode(name, data, len, max_w, max_h, out);
}
