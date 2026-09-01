/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_reader_epub.h"

#include <ctype.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps */

#include "espaperplay_config.h"
#include "zlib.h"

/* LVGL 内置解码库（TJpgD 流式 JPEG / lodepng PNG；主机测试不编译解码器） */

#ifndef ESPAPERPLAY_READER_EPUB_HOST
#include "src/libs/tjpgd/tjpgd.h"
#endif

static const char *TAG = "ESPaperPlay_READER";

/* ====================================================================
 * ZIP 容器层（只解析中央目录，条目按需 inflate）
 * ==================================================================== */

#define EPUB_CACHE_DIR ESPAPERPLAY_SYSTEM_SD_DIR "/cache/reader"
#define EPUB_CACHE_MAGIC 0x43525045u /* "EPRC" */
#define EPUB_PAGEN_VER 2u /* 分页缓存头版本（magic = CACHE_MAGIC + 1） */

#define EPUB_ZIP_EOCD_SIG 0x06054b50UL
#define EPUB_ZIP_CDH_SIG 0x02014b50UL
#define EPUB_ZIP_LFH_SIG 0x04034b50UL
#define EPUB_ZIP_EOCD_SCAN (66 * 1024) /* EOCD 回扫窗口 */

typedef struct {
    uint32_t lfh_off;  /*!< 本地文件头偏移 */
    uint32_t csize;    /*!< 压缩后大小 */
    uint32_t usize;    /* 解压后大小 */
    uint16_t method;   /*!< 0=stored 8=deflate */
    uint32_t name_off; /*!< 名称在名称 blob 内偏移 */
    uint16_t name_len; /*!< 名称长度 */
} epub_zip_ent_t;

typedef struct {
    FILE *fp;
    epub_zip_ent_t *ents;  /*!< 条目表（PSRAM） */
    int ent_cnt;
    char *names;           /*!< 名称 blob（PSRAM） */
    size_t names_len;
} epub_zip_t;

/** 在 [buf, buf+len) 中从尾部向前搜索 EOCD 签名，返回偏移或 -1。 */
static int32_t epub_zip_find_eocd(const uint8_t *buf, size_t len) {
    if (len < 22) {
        return -1;
    }
    size_t i = len - 22;
    while (true) {
        uint32_t sig = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8) |
                       ((uint32_t)buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
        if (sig == EPUB_ZIP_EOCD_SIG) {
            return (int32_t)i;
        }
        if (i == 0) {
            return -1;
        }
        i--;
    }
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** 解析中央目录（文件打开状态下一次性调用）。 */
static esp_err_t epub_zip_parse(epub_zip_t *z) {
    fseek(z->fp, 0, SEEK_END);
    const long fsize = ftell(z->fp);
    if (fsize < 22) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t scan = (size_t)fsize < EPUB_ZIP_EOCD_SCAN ? (size_t)fsize : EPUB_ZIP_EOCD_SCAN;
    uint8_t *scan_buf = heap_caps_malloc(scan, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (scan_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    fseek(z->fp, (long)fsize - (long)scan, SEEK_SET);
    if (fread(scan_buf, 1, scan, z->fp) != scan) {
        heap_caps_free(scan_buf);
        return ESP_FAIL;
    }
    const int32_t eocd = epub_zip_find_eocd(scan_buf, scan);
    if (eocd < 0) {
        heap_caps_free(scan_buf);
        ESP_LOGW(TAG, "epub: EOCD not found");
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint16_t ent_cnt = rd16(&scan_buf[eocd + 10]);
    const uint32_t cd_off = rd32(&scan_buf[eocd + 16]);
    const uint32_t cd_size = rd32(&scan_buf[eocd + 12]);
    heap_caps_free(scan_buf);
    if (ent_cnt == 0xFFFF || cd_off == 0xFFFFFFFFUL) {
        ESP_LOGW(TAG, "epub: ZIP64 not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (ent_cnt > 4096) {
        ESP_LOGW(TAG, "epub: too many entries (%d)", ent_cnt);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *cd = heap_caps_malloc(cd_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    fseek(z->fp, (long)cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, z->fp) != cd_size) {
        heap_caps_free(cd);
        return ESP_FAIL;
    }

    z->ents = heap_caps_calloc(ent_cnt, sizeof(epub_zip_ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* 名称 blob 上界：每条目名 ≤ 512 */
    z->names = heap_caps_malloc((size_t)ent_cnt * 512 + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (z->ents == NULL || z->names == NULL) {
        heap_caps_free(cd);
        return ESP_ERR_NO_MEM;
    }

    size_t np = 0;
    int n = 0;
    size_t p = 0;
    for (int i = 0; i < ent_cnt && p + 46 <= cd_size; i++) {
        if (rd32(&cd[p]) != EPUB_ZIP_CDH_SIG) {
            break;
        }
        const uint16_t method = rd16(&cd[p + 10]);
        const uint32_t csize = rd32(&cd[p + 20]);
        const uint32_t usize = rd32(&cd[p + 24]);
        const uint16_t name_len = rd16(&cd[p + 28]);
        const uint16_t extra_len = rd16(&cd[p + 30]);
        const uint16_t cmt_len = rd16(&cd[p + 32]);
        const uint32_t lfh_off = rd32(&cd[p + 42]);
        if (p + 46 + name_len > cd_size) {
            break;
        }
        if (name_len <= 512) {
            memcpy(&z->names[np], &cd[p + 46], name_len);
            z->ents[n].name_off = (uint32_t)np;
            z->ents[n].name_len = name_len;
            z->ents[n].method = method;
            z->ents[n].csize = csize;
            z->ents[n].usize = usize;
            z->ents[n].lfh_off = lfh_off;
            np += name_len;
            n++;
        }
        p += 46 + (size_t)name_len + extra_len + cmt_len;
    }
    z->ent_cnt = n;
    z->names_len = np;
    heap_caps_free(cd);
    ESP_LOGI(TAG, "epub: zip parsed, %d entries", n);
    return ESP_OK;
}

/* ZIP 访问互斥（递归）：worker 解析与 LVGL 图片解码并发读同一 FILE*；
 * 产出流程持锁期间内嵌的 extract 再次进锁。 */
static SemaphoreHandle_t s_zip_mutex;

static void epub_zip_lock(void) {
    if (s_zip_mutex == NULL) {
        s_zip_mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (s_zip_mutex != NULL) {
        xSemaphoreTakeRecursive(s_zip_mutex, portMAX_DELAY);
    }
}

static void epub_zip_unlock(void) {
    if (s_zip_mutex != NULL) {
        xSemaphoreGiveRecursive(s_zip_mutex);
    }
}

static void epub_zip_close(epub_zip_t *z) {
    epub_zip_lock();
    if (z->fp != NULL) {
        fclose(z->fp);
        z->fp = NULL;
    }
    if (z->ents != NULL) {
        heap_caps_free(z->ents);
        z->ents = NULL;
    }
    if (z->names != NULL) {
        heap_caps_free(z->names);
        z->names = NULL;
    }
    z->ent_cnt = 0;
    epub_zip_unlock();
}

static esp_err_t epub_zip_extract_unlocked(epub_zip_t *z, int idx, char **out_buf,
                                          size_t *out_len);

static esp_err_t epub_zip_extract(epub_zip_t *z, int idx, char **out_buf, size_t *out_len) {
    epub_zip_lock();
    esp_err_t ret = epub_zip_extract_unlocked(z, idx, out_buf, out_len);
    epub_zip_unlock();
    return ret;
}

/** 条目名（指向名称 blob 的临时 NUL 结尾化：复制到调用方缓冲）。 */
static bool epub_zip_ent_name(const epub_zip_t *z, int idx, char *buf, size_t buf_len) {
    const epub_zip_ent_t *e = &z->ents[idx];
    if (e->name_len >= buf_len) {
        return false;
    }
    memcpy(buf, &z->names[e->name_off], e->name_len);
    buf[e->name_len] = '\0';
    return true;
}

/** 按完整路径查找条目（先精确，再大小写不敏感回退）。返回条目序号或 -1。 */
static int epub_zip_find(const epub_zip_t *z, const char *name) {
    char buf[600];
    for (int i = 0; i < z->ent_cnt; i++) {
        if (!epub_zip_ent_name(z, i, buf, sizeof(buf))) {
            continue;
        }
        if (strcmp(buf, name) == 0) {
            return i;
        }
    }
    for (int i = 0; i < z->ent_cnt; i++) {
        if (!epub_zip_ent_name(z, i, buf, sizeof(buf))) {
            continue;
        }
        if (strcasecmp(buf, name) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * 解压条目到 PSRAM 缓冲（NUL 结尾，多分配 1 字节便于文本处理）。
 * inflate 采用 16KB 输入分块流式，输出按 usize 预分配。
 */
static esp_err_t epub_zip_extract_unlocked(epub_zip_t *z, int idx, char **out_buf, size_t *out_len) {
    const epub_zip_ent_t *e = &z->ents[idx];
    if (e->usize > ESPAPERPLAY_EPUB_MAX_ENTRY_BYTES) {
        ESP_LOGW(TAG, "epub: entry too large (%u bytes)", (unsigned)e->usize);
        return ESP_ERR_INVALID_SIZE;
    }
    /* 本地头：签名(4) + 固定(22) 后是 name/extra 长度 */
    uint8_t lfh[30];
    fseek(z->fp, (long)e->lfh_off, SEEK_SET);
    if (fread(lfh, 1, sizeof(lfh), z->fp) != sizeof(lfh) || rd32(lfh) != EPUB_ZIP_LFH_SIG) {
        return ESP_FAIL;
    }
    const uint16_t lname = rd16(&lfh[26]);
    const uint16_t lextra = rd16(&lfh[28]);
    const long data_off = (long)e->lfh_off + 30 + lname + lextra;

    char *out = heap_caps_malloc(e->usize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (e->method == 0) { /* stored */
        fseek(z->fp, data_off, SEEK_SET);
        if (e->usize > 0 && fread(out, 1, e->usize, z->fp) != e->usize) {
            heap_caps_free(out);
            return ESP_FAIL;
        }
    } else if (e->method == 8) { /* deflate（raw） */
        fseek(z->fp, data_off, SEEK_SET);
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, -15) != Z_OK) {
            heap_caps_free(out);
            return ESP_FAIL;
        }
        /* 输入分块缓冲：PSRAM 按需分配（zip 互斥保证串行复用），
         * 避免静态数组占用 16KB 内部 .bss */
        static char *inbuf = NULL;
        static size_t inbuf_cap = 0;
        if (inbuf == NULL) {
            inbuf = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            inbuf_cap = inbuf != NULL ? 16 * 1024 : 1024;
            if (inbuf == NULL) {
                inbuf = heap_caps_malloc(inbuf_cap, MALLOC_CAP_8BIT);
            }
            if (inbuf == NULL) {
                inflateEnd(&zs);
                heap_caps_free(out);
                return ESP_ERR_NO_MEM;
            }
        }
        size_t done = 0;
        int zr = Z_OK;
        while (zr != Z_STREAM_END) {
            size_t chunk = e->csize - (size_t)(zs.total_in);
            if (chunk > inbuf_cap) {
                chunk = inbuf_cap;
            }
            if (chunk == 0) {
                break;
            }
            if (fread(inbuf, 1, chunk, z->fp) != chunk) {
                zr = Z_DATA_ERROR;
                break;
            }
            zs.next_in = (Bytef *)inbuf;
            zs.avail_in = (uInt)chunk;
            while (zs.avail_in > 0 && zr != Z_STREAM_END) {
                zs.next_out = (Bytef *)&out[done];
                zs.avail_out = (uInt)(e->usize - done);
                if (zs.avail_out == 0) {
                    zr = Z_DATA_ERROR; /* 输出超出预期 */
                    break;
                }
                zr = inflate(&zs, Z_NO_FLUSH);
                done = zs.total_out;
                if (zr == Z_BUF_ERROR) {
                    zr = Z_DATA_ERROR;
                    break;
                }
                if (zr != Z_OK && zr != Z_STREAM_END) {
                    break;
                }
            }
        }
        inflateEnd(&zs);
        if (zr != Z_STREAM_END || done != e->usize) {
            ESP_LOGW(TAG, "epub: inflate failed (zr=%d %u/%u)", zr, (unsigned)done,
                     (unsigned)e->usize);
            heap_caps_free(out);
            return ESP_FAIL;
        }
    } else {
        heap_caps_free(out);
        return ESP_ERR_NOT_SUPPORTED;
    }

    out[e->usize] = '\0';
    *out_buf = out;
    if (out_len != NULL) {
        *out_len = e->usize;
    }
    return ESP_OK;
}

/* ====================================================================
 * XML / 路径工具
 * ==================================================================== */

/** 提取标签属性值（原地写入 val，NUL 结尾；找不到返回 false）。 */
static bool xml_attr(const char *tag_start, const char *tag_end, const char *attr, char *val,
                     size_t val_len) {
    const size_t alen = strlen(attr);
    const char *p = tag_start;
    while (p < tag_end) {
        /* 跳到属性名 */
        while (p < tag_end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '/')) {
            p++;
        }
        const char *name_s = p;
        while (p < tag_end && *p != '=' && *p != ' ' && *p != '\t' && *p != '>') {
            p++;
        }
        const size_t nl = (size_t)(p - name_s);
        bool has_val = false;
        const char *vs = NULL;
        size_t vl = 0;
        if (p < tag_end && *p == '=') {
            p++;
            if (p < tag_end && (*p == '"' || *p == '\'')) {
                const char q = *p++;
                vs = p;
                while (p < tag_end && *p != q) {
                    p++;
                }
                vl = (size_t)(p - vs);
                if (p < tag_end) {
                    p++;
                }
            }
            has_val = true;
        }
        if (nl == alen && strncasecmp(name_s, attr, alen) == 0 && has_val && vl < val_len) {
            memcpy(val, vs, vl);
            val[vl] = '\0';
            return true;
        }
    }
    return false;
}

/** URL 百分号解码（原地，返回新长度）。 */
static size_t url_decode(char *s) {
    size_t r = 0, w = 0;
    const size_t n = strlen(s);
    while (r < n) {
        if (s[r] == '%' && r + 2 < n) {
            int hi = -1, lo = -1;
            const char c1 = s[r + 1], c2 = s[r + 2];
            if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
            if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
            if (hi >= 0 && lo >= 0) {
                s[w++] = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        s[w++] = s[r++];
    }
    s[w] = '\0';
    return w;
}

/** 相对路径解析：base_dir + rel（处理 ../ 与 ./，去 #fragment），结果写 resolved。 */
static bool path_resolve(const char *base_dir, const char *rel, char *resolved, size_t rlen) {
    char tmp[600];
    if (rel[0] == '/') {
        strlcpy(tmp, rel + 1, sizeof(tmp)); /* zip 内绝对（少见） */
    } else {
        snprintf(tmp, sizeof(tmp), "%s/%s", base_dir, rel);
    }
    char *frag = strchr(tmp, '#');
    if (frag != NULL) {
        *frag = '\0';
    }
    /* 词法归一：按 '/' 分段，"." 丢弃，".." 弹出上一段 */
    char out[600];
    size_t op = 0;
    const size_t n = strlen(tmp);
    size_t i = 0;
    while (i <= n) {
        /* 取一段 [i, e) */
        size_t e = i;
        while (e < n && tmp[e] != '/') {
            e++;
        }
        const size_t len = e - i;
        if (len == 0 || (len == 1 && tmp[i] == '.')) {
            /* 空段 / "."：丢弃 */
        } else if (len == 2 && tmp[i] == '.' && tmp[i + 1] == '.') {
            /* ".."：弹出 out 的最后一段 */
            while (op > 0 && out[op - 1] != '/') {
                op--;
            }
            if (op > 0) {
                op--;
            }
        } else {
            if (op + len + 1 >= sizeof(out)) {
                return false;
            }
            if (op > 0) {
                out[op++] = '/';
            }
            memcpy(&out[op], &tmp[i], len);
            op += len;
        }
        if (e >= n) {
            break;
        }
        i = e + 1;
    }
    out[op] = '\0';
    if (op == 0 || op >= rlen) {
        return false;
    }
    memcpy(resolved, out, op + 1);
    return true;
}

/** 取路径的目录部分（不含尾斜杠；根目录返回 "."）。 */
static void path_dir(const char *path, char *dir, size_t dlen) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path) {
        strlcpy(dir, ".", dlen);
    } else {
        const size_t nl = (size_t)(slash - path);
        if (nl >= dlen) {
            strlcpy(dir, ".", dlen);
            return;
        }
        memcpy(dir, path, nl);
        dir[nl] = '\0';
    }
}

/* ====================================================================
 * OPF / 书目
 * ==================================================================== */

typedef struct {
    char *zip_path;  /*!< 压缩包内完整路径（堆分配） */
    uint8_t kind;    /*!< 0=其他 1=xhtml 2=图片 3=字体 */
} epub_item_t;

static struct {
    epub_zip_t zip;
    bool open;
    uint32_t token; /* 书指纹（路径哈希 ^ mtime ^ size）：缓存与预取槽校验 */
    char title[128];      /*!< dc:title */
    char opf_dir[300];    /*!< OPF 所在目录 */
    epub_item_t *items;   /*!< manifest */
    int item_cnt;
    int *spine;           /*!< spine → item 序号 */
    int spine_cnt;
    /* 驻留章节 */
    int cur_ch;           /*!< -1=未驻留 */
    char *ch_text;        /*!< 文本 blob */
    size_t ch_text_len;
    char ch_title[128];
    espaperplay_reader_block_t *ch_blocks;
    int ch_block_cnt;
    int *ch_images;       /*!< 块图片 id → manifest item 序号 */
    int ch_image_cnt;
    /* 图片缓存（单张） */
    int img_cached_id;
    uint8_t *img_buf;
    lv_image_dsc_t img_dsc;
} s_epub;

/** 媒体类型归类。 */
static uint8_t epub_media_kind(const char *mime, const char *zip_path) {
    if (strstr(mime, "xhtml") != NULL || strstr(mime, "html") != NULL) {
        return 1;
    }
    if (strstr(mime, "image/") == mime) {
        return 2;
    }
    if (strstr(mime, "font") != NULL || strstr(zip_path, ".ttf") != NULL ||
        strstr(zip_path, ".otf") != NULL) {
        return 3;
    }
    return 0;
}

/** 解析 OPF（manifest + spine + dc:title）。 */
static esp_err_t epub_parse_opf(char *opf) { /* opf 可原地修改 */
    const size_t len = strlen(opf);
    /* 书名：首个 <dc:title>…</dc:title> */
    char *tp = strcasestr(opf, "<dc:title");
    if (tp != NULL) {
        char *ts = strchr(tp, '>');
        char *te = ts != NULL ? strcasestr(ts, "</dc:title") : NULL;
        if (ts != NULL && te != NULL && te > ts + 1 && (size_t)(te - ts - 1) < sizeof(s_epub.title)) {
            memcpy(s_epub.title, ts + 1, (size_t)(te - ts - 1));
            s_epub.title[te - ts - 1] = '\0';
        }
    }

    /* manifest：逐个 <item .../> */
    int cap = 64;
    s_epub.items = heap_caps_calloc(cap, sizeof(epub_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_epub.items == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const char *p = opf;
    int font_cnt = 0;
    while ((p = strcasestr(p, "<item")) != NULL) {
        const char *te = strchr(p, '>');
        if (te == NULL) {
            break;
        }
        if (te[-1] == '?' || te[-1] == '/') {
            te--; /* 自闭合尾 */
        }
        char href[600];
        char mime[64];
        if (xml_attr(p, te, "href", href, sizeof(href)) &&
            xml_attr(p, te, "media-type", mime, sizeof(mime))) {
            url_decode(href);
            char zp[600];
            if (path_resolve(s_epub.opf_dir, href, zp, sizeof(zp))) {
                const uint8_t kind = epub_media_kind(mime, zp);
                if (kind != 0) {
                    if (s_epub.item_cnt >= cap) {
                        const int nc = cap * 2;
                        epub_item_t *ni = heap_caps_realloc(
                            s_epub.items, (size_t)nc * sizeof(epub_item_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (ni == NULL) {
                            break;
                        }
                        s_epub.items = ni;
                        cap = nc;
                    }
                    epub_item_t *it = &s_epub.items[s_epub.item_cnt];
                    it->zip_path = heap_caps_malloc(strlen(zp) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (it->zip_path != NULL) {
                        strcpy(it->zip_path, zp);
                        it->kind = kind;
                        s_epub.item_cnt++;
                        if (kind == 3) {
                            font_cnt++;
                        }
                    }
                }
            }
        }
        p = te + 1;
    }
    if (s_epub.item_cnt == 0) {
        return ESP_FAIL;
    }
    if (font_cnt > 0) {
        ESP_LOGI(TAG, "epub: %d embedded font(s) parsed (using system font by default)", font_cnt);
    }

    /* spine：按 itemref idref 顺序映射 manifest 条目 */
    const char *mstart = strcasestr(opf, "<spine");
    if (mstart == NULL) {
        return ESP_FAIL;
    }
    const char *mend = strcasestr(mstart, "</spine");
    if (mend == NULL) {
        mend = opf + len;
    }
    int scap = 64;
    s_epub.spine = heap_caps_calloc(scap, sizeof(int), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_epub.spine == NULL) {
        return ESP_ERR_NO_MEM;
    }
    p = mstart;
    while (p < mend && (p = strcasestr(p, "<itemref")) != NULL && p < mend) {
        const char *te = strchr(p, '>');
        if (te == NULL || te > mend) {
            break;
        }
        const char *xe = te[-1] == '/' ? te - 1 : te;
        char idref[128];
        if (xml_attr(p, xe, "idref", idref, sizeof(idref))) {
            /* idref → 找 id 属性匹配的 xhtml item */
            /* 需要 id：回扫一次 manifest（id 与 href 常成对，重新解析 id） */
            const char *q = opf;
            int found = -1;
            int idx = -1;
            while ((q = strcasestr(q, "<item")) != NULL) {
                const char *qe = strchr(q, '>');
                if (qe == NULL) {
                    break;
                }
                const char *qxe = qe[-1] == '/' ? qe - 1 : qe;
                char id[128];
                char href[600];
                if (xml_attr(q, qxe, "href", href, sizeof(href))) {
                    url_decode(href);
                    char zp[600];
                    if (path_resolve(s_epub.opf_dir, href, zp, sizeof(zp))) {
                        for (int i = 0; i < s_epub.item_cnt; i++) {
                            if (strcmp(s_epub.items[i].zip_path, zp) == 0) {
                                idx = i;
                                break;
                            }
                        }
                        if (idx >= 0 && xml_attr(q, qxe, "id", id, sizeof(id)) &&
                            strcmp(id, idref) == 0 && s_epub.items[idx].kind == 1) {
                            found = idx;
                            break;
                        }
                    }
                }
                q = qe + 1;
            }
            if (found >= 0) {
                if (s_epub.spine_cnt >= scap) {
                    const int nc = scap * 2;
                    int *ns = heap_caps_realloc(s_epub.spine, (size_t)nc * sizeof(int),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (ns == NULL) {
                        break;
                    }
                    s_epub.spine = ns;
                    scap = nc;
                }
                s_epub.spine[s_epub.spine_cnt++] = found;
            }
        }
        p = te + 1;
    }
    return s_epub.spine_cnt > 0 ? ESP_OK : ESP_FAIL;
}

/* ====================================================================
 * CSS 子集解析（类规则：对齐 / 粗斜体 / 首行缩进）
 * ====================================================================
 *
 * 完整 CSS 渲染超出墨水屏设备能力，这里只解析影响「阅读版式」的最小集合：
 *   - 类规则 `.name { text-align: center|right; font-weight: bold;
 *     font-style: italic; text-indent: 0 }`
 *   - 元素规则 `p { text-indent: 2em }`（CJK 小说通用正文首行缩进）
 * 字体族 / 颜色 / 阴影 / 逐字艺术字（按 span 变字号）不还原：BW 墨水屏
 * 无颜色，内嵌字体多为缺字子集，正文以系统字库渲染。
 */

#define CSSF_CENTER 0x01
#define CSSF_RIGHT 0x02
#define CSSF_BOLD 0x04
#define CSSF_ITALIC 0x08
#define CSSF_NOINDENT 0x10

#define CSS_CLASS_MAX 96  /*!< 类规则表容量 */
#define CSS_CLASS_NAME 32 /*!< 类名截断长度 */

typedef struct {
    char name[CSS_CLASS_NAME];
    uint8_t flags;
} epub_css_class_t;

/** CSS 解析状态（按产出者各持一份——worker 与 LVGL 线程互不共享）。 */
typedef struct {
    epub_css_class_t classes[CSS_CLASS_MAX];
    int cnt;
    bool p_indent;   /*!< 元素规则 p{text-indent≥1em}：正文首行缩进 */
} epub_css_t;

static void epub_css_init(epub_css_t *c) {
    memset(c, 0, sizeof(*c));
}

/** 解析一段声明文本（"text-align: center; font-weight: bold; ..."）。 */
static uint8_t epub_css_decls(const char *decls, size_t len, bool *indent_on, bool *indent_off) {
    uint8_t flags = 0;
    const char *p = decls;
    const char *end = decls + len;
    while (p < end) {
        const char *semi = memchr(p, ';', (size_t)(end - p));
        const char *stop = semi != NULL ? semi : end;
        const char *colon = memchr(p, ':', (size_t)(stop - p));
        if (colon != NULL && colon + 1 < stop) {
            const char *vn = p;
            size_t vnl = (size_t)(colon - p);
            while (vnl > 0 && (*vn == ' ' || *vn == '\t' || *vn == '\n' || *vn == '\r')) {
                vn++;
                vnl--;
            }
            const char *vv = colon + 1;
            size_t vvl = (size_t)(stop - vv);
            while (vvl > 0 && (*vv == ' ' || *vv == '\t' || *vv == '\n' || *vv == '\r')) {
                vv++;
                vvl--;
            }
            if (vnl == 10 && strncasecmp(vn, "text-align", 10) == 0) {
                if (vvl >= 6 && strncasecmp(vv, "center", 6) == 0) {
                    flags |= CSSF_CENTER;
                } else if (vvl >= 5 && strncasecmp(vv, "right", 5) == 0) {
                    flags |= CSSF_RIGHT;
                }
            } else if (vnl == 11 && strncasecmp(vn, "font-weight", 11) == 0) {
                if (vvl >= 4 && strncasecmp(vv, "bold", 4) == 0) {
                    flags |= CSSF_BOLD;
                } else if (vvl >= 3 && vv[0] >= '4' && vv[0] <= '9') {
                    flags |= CSSF_BOLD; /* 600/700/800/900 */
                }
            } else if (vnl == 10 && strncasecmp(vn, "font-style", 10) == 0) {
                if (vvl >= 6 && strncasecmp(vv, "italic", 6) == 0) {
                    flags |= CSSF_ITALIC;
                }
            } else if (vnl == 11 && strncasecmp(vn, "text-indent", 11) == 0) {
                if (vvl >= 1 && vv[0] == '0') {
                    if (indent_off != NULL) {
                        *indent_off = true;
                    }
                } else {
                    if (indent_on != NULL) {
                        *indent_on = true;
                    }
                }
            }
        }
        p = semi != NULL ? semi + 1 : end;
    }
    return flags;
}

/** 类名入表（已存在则 OR flags）。 */
static void epub_css_class_add(epub_css_t *c, const char *name, size_t nlen, uint8_t flags) {
    if (nlen == 0 || nlen >= CSS_CLASS_NAME || flags == 0) {
        return;
    }
    for (int i = 0; i < c->cnt; i++) {
        if (strlen(c->classes[i].name) == nlen && strncmp(c->classes[i].name, name, nlen) == 0) {
            c->classes[i].flags |= flags;
            return;
        }
    }
    if (c->cnt >= CSS_CLASS_MAX) {
        return;
    }
    memcpy(c->classes[c->cnt].name, name, nlen);
    c->classes[c->cnt].name[nlen] = '\0';
    c->classes[c->cnt].flags = flags;
    c->cnt++;
}

/** 解析整份样式表（选择器 { 声明 } 的朴素扫描）。 */
static void epub_css_parse(epub_css_t *c, char *css) {
    char *p = css;
    while ((p = strchr(p, '{')) != NULL) {
        char *close = strchr(p, '}');
        if (close == NULL) {
            break;
        }
        /* 选择器：上一个 '}' / '{' / 行首 到 '{' */
        char *sel_start = p;
        while (sel_start > css && *(sel_start - 1) != '}' && *(sel_start - 1) != '{' &&
               *(sel_start - 1) != '\n') {
            sel_start--;
        }
        bool indent_on = false, indent_off = false;
        const uint8_t flags =
            epub_css_decls(p + 1, (size_t)(close - p - 1), &indent_on, &indent_off);
        /* 类选择器（.a.b 级联 / .a .b 后代）：抽取全部 .name */
        bool has_class = false;
        for (char *s = sel_start; s < p; s++) {
            if (*s == '.' && s + 1 < p) {
                char *n = s + 1;
                size_t nl = 0;
                while (&n[nl] < p &&
                       (isalnum((unsigned char)n[nl]) || n[nl] == '-' || n[nl] == '_')) {
                    nl++;
                }
                if (nl > 0) {
                    uint8_t f = flags;
                    if (indent_off) {
                        f |= CSSF_NOINDENT;
                    }
                    epub_css_class_add(c, n, nl, f);
                    has_class = true;
                }
                s = &n[nl];
            }
        }
        /* 裸元素规则 p：仅关心 text-indent（正文首行缩进开关） */
        if (!has_class && (indent_on || indent_off)) {
            for (char *s = sel_start; s < p; s++) {
                if ((*s == 'p' || *s == 'P') &&
                    (s == sel_start || isspace((unsigned char)*(s - 1))) &&
                    (s + 1 == p || isspace((unsigned char)s[1]))) {
                    if (indent_on && !indent_off) {
                        c->p_indent = true;
                    }
                    break;
                }
            }
        }
        p = close + 1;
    }
    ESP_LOGD(TAG, "epub: css parsed, %d class rule(s), p_indent=%d", c->cnt, c->p_indent);
}

/** 查类规则（class 属性可含多个类名，任一命中即 OR）。 */
static uint8_t epub_css_lookup(epub_css_t *c, const char *class_attr) {
    if (class_attr == NULL || c->cnt == 0) {
        return 0;
    }
    uint8_t flags = 0;
    const char *p = class_attr;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        const char *n = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        const size_t nl = (size_t)(p - n);
        if (nl == 0) {
            break;
        }
        for (int i = 0; i < c->cnt; i++) {
            if (strlen(c->classes[i].name) == nl && strncmp(c->classes[i].name, n, nl) == 0) {
                flags |= c->classes[i].flags;
            }
        }
    }
    return flags;
}

/* ====================================================================
 * 章节数据包（worker 产出 / LVGL 采纳；解析结果与装载解耦）
 * ==================================================================== */

/** 章节解析结果包：text/blocks/images 指针所有权归属包，采纳后移交 live 状态。 */
typedef struct {
    uint32_t token;  /*!< 产出时的书指纹（采纳校验，防换书串包） */
    int chapter;     /*!< 章节号（-1=空包） */
    char title[128]; /*!< XHTML <title>（实体已反转义） */
    char *text;      /*!< 文本 blob（PSRAM，NUL 结尾） */
    size_t text_len;
    espaperplay_reader_block_t *blocks;
    int block_cnt;
    int *images; /*!< 块图片 id → zip 条目序号 */
    int image_cnt;
} epub_packet_t;

static void epub_packet_free(epub_packet_t *pkt) {
    if (pkt == NULL) {
        return;
    }
    heap_caps_free(pkt->text);
    heap_caps_free(pkt->blocks);
    heap_caps_free(pkt->images);
    memset(pkt, 0, sizeof(*pkt));
    pkt->chapter = -1;
}

/* ====================================================================
 * XHTML → 块模型（单遍状态机）
 * ==================================================================== */

#define XS_SKIP_TAGS                                                                                   \
    "style", "script", "title", "head", "rt", "rp", "svg", "nav"

/** 块级样式上下文（随块级标签开合入栈/出栈，段落继承栈顶样式）。 */
typedef struct {
    uint8_t align;     /*!< 0=左 1=中 2=右 */
    uint8_t head;      /*!< 标题层级 0..6（作用于标题内全部段落） */
    bool no_indent;    /*!< text-indent:0（本层禁用首行缩进） */
} xs_ctx_t;

#define XS_CTX_MAX 12

typedef struct {
    char *blob;         /*!< 文本 blob（PSRAM，增长） */
    size_t blob_len, blob_cap;
    espaperplay_reader_block_t *blocks;
    int blk_cnt, blk_cap;
    int *images;        /*!< 图片 id → manifest item 序号 */
    int img_cnt, img_cap;
    /* 段落累积 */
    uint32_t para_off;
    uint32_t para_len;
    uint16_t para_flags;
    int skip_depth;     /*!< style/script/... 内嵌套深度 */
    const char *skip_tag;
    int bold_depth, italic_depth, quote_depth;
    bool para_any;      /*!< 当前段落已有文本 */
    bool para_indented; /*!< 当前段落已插入首行缩进 */
    xs_ctx_t ctx[XS_CTX_MAX]; /*!< 块级样式栈（ctx[0]=基础） */
    int ctx_top;
    epub_css_t *css; /*!< 章节样式表（产出者持有） */
} xhtml_state_t;

/** 标签是否为块级（决定样式栈开合与段落切分）。 */
static bool xs_is_block_tag(const char *name) {
    static const char *block_tags[] = {"p",     "div",  "li",    "tr",     "section",
                                       "article", "ul",  "ol",   "table",  "header",
                                       "footer", "aside", "h1",  "h2",     "h3",
                                       "h4",    "h5",   "h6",   "figcaption"};
    for (size_t i = 0; i < sizeof(block_tags) / sizeof(block_tags[0]); i++) {
        if (strcmp(name, block_tags[i]) == 0) {
            return true;
        }
    }
    return false;
}

/** 追加 UTF-8 字节到 blob，失败返回 false。 */
static bool xs_putb(xhtml_state_t *st, char c) {
    if (st->blob_len + 1 >= st->blob_cap) {
        const size_t nc = st->blob_cap < 4096 ? 4096 : st->blob_cap * 2;
        char *nb = heap_caps_realloc(st->blob, nc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb == NULL) {
            return false;
        }
        st->blob = nb;
        st->blob_cap = nc;
    }
    st->blob[st->blob_len++] = c;
    return true;
}

/** 追加字符序列。 */
static bool xs_puts(xhtml_state_t *st, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!xs_putb(st, s[i])) {
            return false;
        }
    }
    return true;
}

/** 压入块。 */
static bool xs_push_block(xhtml_state_t *st, uint32_t off, uint32_t len, uint16_t flags,
                          int image) {
    if (st->blk_cnt >= st->blk_cap) {
        const int nc = st->blk_cap < 64 ? 64 : st->blk_cap * 2;
        espaperplay_reader_block_t *nb =
            heap_caps_realloc(st->blocks, (size_t)nc * sizeof(*nb), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb == NULL) {
            return false;
        }
        st->blocks = nb;
        st->blk_cap = nc;
    }
    espaperplay_reader_block_t *b = &st->blocks[st->blk_cnt++];
    b->off = off;
    b->len = len;
    b->flags = flags;
    b->image = image;
    return true;
}

/** 结束当前文本段（有内容时压块）。 */
static bool xs_flush_para(xhtml_state_t *st) {
    if (st->para_any && st->para_len > 0) {
        uint16_t fl = st->para_flags;
        const xs_ctx_t *cx = &st->ctx[st->ctx_top];
        if (cx->align == 1) {
            fl |= ESPAPERPLAY_READER_BLK_CENTER;
        } else if (cx->align == 2) {
            fl |= ESPAPERPLAY_READER_BLK_RIGHT;
        }
        if (cx->head > 0) {
            fl |= (uint16_t)cx->head << ESPAPERPLAY_READER_BLK_HEAD_SHIFT;
        }
        if (st->quote_depth > 0) {
            fl |= ESPAPERPLAY_READER_BLK_QUOTE;
        }
        if (st->para_indented) {
            fl |= ESPAPERPLAY_READER_BLK_INDENTED;
        }
        if (!xs_push_block(st, st->para_off, st->para_len, fl, -1)) {
            return false;
        }
    }
    st->para_len = 0;
    st->para_any = false;
    st->para_flags = 0;
    st->para_indented = false;
    st->para_off = st->blob_len; /* 下一块起点 = 当前 blob 末尾 */
    return true;
}

/** 块级标签开样式栈：合并 class / style / align 属性到新栈层。 */
static void xs_ctx_open(xhtml_state_t *st, const char *name, const char *tag_s, const char *tag_e) {
    xs_ctx_t nc = st->ctx[st->ctx_top];
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
        nc.head = (uint8_t)(name[1] - '0');
    }
    uint8_t fl = 0;
    char attr[256];
    if (xml_attr(tag_s, tag_e, "class", attr, sizeof(attr))) {
        fl |= epub_css_lookup(st->css, attr);
    }
    if (xml_attr(tag_s, tag_e, "style", attr, sizeof(attr))) {
        bool ind_on = false, ind_off = false;
        fl |= epub_css_decls(attr, strlen(attr), &ind_on, &ind_off);
        if (ind_off) {
            fl |= CSSF_NOINDENT;
        }
    }
    if (xml_attr(tag_s, tag_e, "align", attr, sizeof(attr))) {
        if (strcasecmp(attr, "center") == 0) {
            fl |= CSSF_CENTER;
        } else if (strcasecmp(attr, "right") == 0) {
            fl |= CSSF_RIGHT;
        }
    }
    if ((fl & CSSF_CENTER) != 0) {
        nc.align = 1;
    } else if ((fl & CSSF_RIGHT) != 0) {
        nc.align = 2;
    }
    if ((fl & CSSF_NOINDENT) != 0) {
        nc.no_indent = true;
    }
    /* class/style 的粗斜体作用于整段 */
    if ((fl & CSSF_BOLD) != 0) {
        st->para_flags |= ESPAPERPLAY_READER_BLK_BOLD;
    }
    if ((fl & CSSF_ITALIC) != 0) {
        st->para_flags |= ESPAPERPLAY_READER_BLK_ITALIC;
    }
    if (st->ctx_top < XS_CTX_MAX - 1) {
        st->ctx[++st->ctx_top] = nc;
    } else {
        st->ctx[st->ctx_top] = nc; /* 栈满：原位覆盖（深嵌套退化） */
    }
}

/** 块级标签关样式栈。 */
static void xs_ctx_close(xhtml_state_t *st) {
    if (st->ctx_top > 0) {
        st->ctx_top--;
    }
}

/** 压入图片块（manifest item 序号）。 */
static bool xs_push_image(xhtml_state_t *st, int item_idx) {
    if (!xs_flush_para(st)) {
        return false;
    }
    if (st->img_cnt >= st->img_cap) {
        const int nc = st->img_cap < 8 ? 8 : st->img_cap * 2;
        int *ni = heap_caps_realloc(st->images, (size_t)nc * sizeof(int),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ni == NULL) {
            return false;
        }
        st->images = ni;
        st->img_cap = nc;
    }
    st->images[st->img_cnt] = item_idx;
    return xs_push_block(st, 0, 0, 0, st->img_cnt++);
}

/** 块级开始标签处理（返回 false=内存耗尽）。 */
static bool xs_open_tag(xhtml_state_t *st, const char *name, const char *tag_s, const char *tag_e,
                        const char *base_dir) {
    static const char *skip_tags[] = {XS_SKIP_TAGS};
    for (size_t i = 0; i < sizeof(skip_tags) / sizeof(skip_tags[0]); i++) {
        if (strcmp(name, skip_tags[i]) == 0) {
            if (st->skip_depth == 0) {
                st->skip_tag = skip_tags[i];
                st->skip_depth = 1;
            } else if (strcmp(name, st->skip_tag) == 0) {
                st->skip_depth++;
            }
            return true;
        }
    }
    if (st->skip_depth > 0) {
        /* SVG 内的 <image xlink:href> 仍需提取（常见整页插图写法） */
        if (st->skip_tag != NULL && strcmp(st->skip_tag, "svg") == 0 &&
            (strcmp(name, "image") == 0 || strcmp(name, "img") == 0)) {
            char src[600];
            if (xml_attr(tag_s, tag_e, "xlink:href", src, sizeof(src)) ||
                xml_attr(tag_s, tag_e, "src", src, sizeof(src))) {
                url_decode(src);
                char zp[600];
                if (path_resolve(base_dir, src, zp, sizeof(zp))) {
                    const int zi = epub_zip_find(&s_epub.zip, zp);
                    if (zi >= 0) {
                        return xs_push_image(st, zi);
                    }
                    ESP_LOGD(TAG, "epub: image not found: %s", zp);
                }
            }
        }
        return true;
    }

    if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0) {
        st->bold_depth++;
        st->para_flags |= ESPAPERPLAY_READER_BLK_BOLD;
        return true;
    }
    if (strcmp(name, "i") == 0 || strcmp(name, "em") == 0) {
        st->italic_depth++;
        st->para_flags |= ESPAPERPLAY_READER_BLK_ITALIC;
        return true;
    }
    if (strcmp(name, "blockquote") == 0) {
        st->quote_depth++;
        return true;
    }
    if (strcmp(name, "br") == 0) {
        st->para_any = true; /* 空行也可视作段落内容（保留换行结构） */
        return xs_putb(st, '\n');
    }
    if (strcmp(name, "img") == 0 || strcmp(name, "image") == 0) {
        char src[600];
        if (!xml_attr(tag_s, tag_e, "src", src, sizeof(src)) &&
            !xml_attr(tag_s, tag_e, "xlink:href", src, sizeof(src))) {
            return true; /* 无 src：忽略 */
        }
        url_decode(src);
        char zp[600];
        if (!path_resolve(base_dir, src, zp, sizeof(zp))) {
            return true;
        }
        const int zi = epub_zip_find(&s_epub.zip, zp);
        if (zi < 0) {
            ESP_LOGD(TAG, "epub: image not found: %s", zp);
            return true;
        }
        /* 图片块持有 zip 条目序号（避免经由 manifest 间接查找） */
        return xs_push_image(st, zi);
    }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
        if (!xs_flush_para(st)) {
            return false;
        }
        xs_ctx_open(st, name, tag_s, tag_e);
        return true;
    }
    /* 其余块级标签：结束当前段 + 开样式栈（class/style/align 继承给内部段落） */
    if (xs_is_block_tag(name)) {
        if (!xs_flush_para(st)) {
            return false;
        }
        xs_ctx_open(st, name, tag_s, tag_e);
        return true;
    }
    return true;
}

/** 块级结束标签处理。 */
static bool xs_close_tag(xhtml_state_t *st, const char *name) {
    if (st->skip_depth > 0) {
        if (strcmp(name, st->skip_tag) == 0) {
            st->skip_depth--;
            if (st->skip_depth == 0) {
                st->skip_tag = NULL;
            }
        }
        return true;
    }
    if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0) {
        if (st->bold_depth > 0) {
            st->bold_depth--;
        }
        return true;
    }
    if (strcmp(name, "i") == 0 || strcmp(name, "em") == 0) {
        if (st->italic_depth > 0) {
            st->italic_depth--;
        }
        return true;
    }
    if (strcmp(name, "blockquote") == 0) {
        if (st->quote_depth > 0) {
            st->quote_depth--;
        }
        return true;
    }
    /* 块级标签：结束当前段 + 关样式栈 */
    if (strcmp(name, "blockquote") == 0 || xs_is_block_tag(name)) {
        if (!xs_flush_para(st)) {
            return false;
        }
        if (strcmp(name, "blockquote") != 0) {
            xs_ctx_close(st);
        }
        return true;
    }
    return true;
}

/** 文本段处理：实体解码 + 空白折叠。 */
static bool xs_text(xhtml_state_t *st, const char *s, size_t n) {
    if (st->skip_depth > 0) {
        return true;
    }
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '&') {
            /* 实体解码 */
            char dec[8];
            size_t dl = 0;
            if (s[i + 1] == '#') {
                /* 数值实体：&#123; / &#x1F600;（向后找 ';'，最多 10 字符） */
                size_t j = i + 2;
                while (j < n && s[j] != ';' && s[j] != '<' && j - i < 12) {
                    j++;
                }
                if (j >= n || s[j] != ';') {
                    i++;
                    continue; /* 非法实体：跳过 '&' */
                }
                char numbuf[12];
                const size_t nl2 = j - (i + 2);
                if (nl2 == 0 || nl2 >= sizeof(numbuf)) {
                    i = j + 1;
                    continue;
                }
                memcpy(numbuf, &s[i + 2], nl2);
                numbuf[nl2] = '\0';
                long cp = (numbuf[0] == 'x' || numbuf[0] == 'X') ? strtol(&numbuf[1], NULL, 16)
                                                                  : strtol(numbuf, NULL, 10);
                if (cp > 0 && cp < 0x110000) {
                    if (cp < 0x80) {
                        dec[dl++] = (char)cp;
                    } else if (cp < 0x800) {
                        dec[dl++] = (char)(0xC0 | (cp >> 6));
                        dec[dl++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        dec[dl++] = (char)(0xE0 | (cp >> 12));
                        dec[dl++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        dec[dl++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        dec[dl++] = (char)(0xF0 | (cp >> 18));
                        dec[dl++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        dec[dl++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        dec[dl++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                i = j + 1;
                if (dl == 0) {
                    continue;
                }
                st->para_any = true;
                st->para_len += dl;
                if (!xs_puts(st, dec, dl)) {
                    return false;
                }
                continue;
            }
            const char *semi = memchr(&s[i], ';', n - i < 8 ? n - i : 8);
            if (semi != NULL) {
                const size_t el = (size_t)(semi - &s[i]);
                if (strncmp(&s[i], "&amp;", 5) == 0) {
                    dec[dl++] = '&';
                } else if (strncmp(&s[i], "&lt;", 4) == 0) {
                    dec[dl++] = '<';
                } else if (strncmp(&s[i], "&gt;", 4) == 0) {
                    dec[dl++] = '>';
                } else if (strncmp(&s[i], "&quot;", 6) == 0) {
                    dec[dl++] = '"';
                } else if (strncmp(&s[i], "&apos;", 6) == 0) {
                    dec[dl++] = '\'';
                } else if (strncmp(&s[i], "&nbsp;", 6) == 0) {
                    dec[dl++] = ' ';
                } else {
                    i += el + 1;
                    continue; /* 未知实体：丢弃 */
                }
                i += el + 1;
                st->para_any = true;
                st->para_len += dl;
                if (!xs_puts(st, dec, dl)) {
                    return false;
                }
                continue;
            }
        }
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            /* 折叠为单空格：段首或紧跟 '\n' 时不加 */
            const char prev = st->blob_len > st->para_off ? st->blob[st->blob_len - 1] : '\0';
            if (prev != '\0' && prev != '\n' && prev != ' ' && st->para_len > 0) {
                st->para_len++;
                if (!xs_putb(st, ' ')) {
                    return false;
                }
            }
            i++;
            continue;
        }
        /* 段落首个实字符：按 CSS 基础规则插入 CJK 首行缩进（两个全角空格） */
        if (st->para_len == 0 && !st->para_indented && st->css != NULL && st->css->p_indent) {
            const xs_ctx_t *cx = &st->ctx[st->ctx_top];
            if (!cx->no_indent && cx->align == 0 && cx->head == 0) {
                static const char indent[7] = {'\xE3', '\x80', '\x80', '\xE3', '\x80', '\x80', '\0'};
                if (!xs_puts(st, indent, 6)) {
                    return false;
                }
                st->para_len += 6;
                st->para_indented = true;
            }
        }
        st->para_any = true;
        st->para_len++;
        if (!xs_putb(st, c)) {
            return false;
        }
        i++;
    }
    return true;
}

/** XHTML 主解析（html 为解压后的可修改缓冲；结果写入 packet）。 */
static esp_err_t epub_parse_xhtml(char *html, const char *zip_path, epub_css_t *css,
                                  epub_packet_t *out) {
    xhtml_state_t st = {0};
    st.para_off = 0;
    st.blocks = NULL;
    st.blk_cnt = 0;
    st.blk_cap = 0;
    st.css = css;
    char base_dir[300];
    path_dir(zip_path, base_dir, sizeof(base_dir));

    const size_t n = strlen(html);
    size_t i = 0;
    while (i < n) {
        if (html[i] == '<') {
            if (strncmp(&html[i], "<!--", 4) == 0) {
                const char *end = strstr(&html[i], "-->");
                i = end != NULL ? (size_t)(end - html) + 3 : n;
                continue;
            }
            if (strncmp(&html[i], "<?", 2) == 0) {
                const char *end = strstr(&html[i], "?>");
                i = end != NULL ? (size_t)(end - html) + 2 : n;
                continue;
            }
            if (strncmp(&html[i], "<!", 2) == 0) {
                const char *end = strchr(&html[i], '>');
                i = end != NULL ? (size_t)(end - html) + 1 : n;
                continue;
            }
            const char *te = strchr(&html[i], '>');
            if (te == NULL) {
                break;
            }
            const size_t tl = (size_t)(te - &html[i]);
            bool closing = html[i + 1] == '/';
            size_t ns = closing ? i + 2 : i + 1;
            /* 标签名（小写，截断到 15） */
            char name[16];
            size_t nl = 0;
            while (ns + nl < i + tl && html[ns + nl] != ' ' && html[ns + nl] != '\t' &&
                   html[ns + nl] != '\n' && html[ns + nl] != '\r' && html[ns + nl] != '/' &&
                   nl < sizeof(name) - 1) {
                name[nl] = (char)tolower((unsigned char)html[ns + nl]);
                nl++;
            }
            name[nl] = '\0';
            if (nl == 0) {
                i = (size_t)(te - html) + 1;
                continue;
            }
            /* 自闭合：/> */
            const bool self_close = te[-1] == '/';
            const char *tag_e = self_close ? te - 1 : te;
            if (!self_close) {
                if (closing ? !xs_close_tag(&st, name)
                            : !xs_open_tag(&st, name, &html[i], tag_e, base_dir)) {
                    goto oom;
                }
            } else {
                /* 自闭合：open + close */
                if (!xs_open_tag(&st, name, &html[i], tag_e, base_dir) ||
                    !xs_close_tag(&st, name)) {
                    goto oom;
                }
            }
            i = (size_t)(te - html) + 1;
            continue;
        }
        /* 文本段 */
        const char *te = strchr(&html[i], '<');
        const size_t tl = te != NULL ? (size_t)(te - &html[i]) : n - i;
        if (!xs_text(&st, &html[i], tl)) {
            goto oom;
        }
        i += tl;
    }
    xs_flush_para(&st);

    /* NUL 结尾 */
    if (!xs_putb(&st, '\0')) {
        goto oom;
    }
    out->text = st.blob;
    out->text_len = st.blob_len ? st.blob_len - 1 : 0;
    out->blocks = st.blocks;
    out->block_cnt = st.blk_cnt;
    out->images = st.images;
    out->image_cnt = st.img_cnt;
    return ESP_OK;
oom:
    heap_caps_free(st.blob);
    heap_caps_free(st.blocks);
    heap_caps_free(st.images);
    return ESP_ERR_NO_MEM;
}

/* ====================================================================
 * 图片解码（PNG zlib 流式逐行 / JPEG TJpgD 流式，解码中抽样 → RGB565）
 * ==================================================================== */

/** 提取 1/2/4 位子字节中的一个值。 */
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
                    a = px[off + 2];
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
            const uint8_t idx =
                (bd == 8) ? px[sx] : png_subbyte(px, (size_t)sx * bd, bd);
            if ((size_t)idx * 3 + 2 < plte_len) {
                r = plte[idx * 3];
                g = plte[idx * 3 + 1];
                b = plte[idx * 3 + 2];
            }
            if (trns_len > idx && trns[idx] < 128) {
                a = 0; /* 透明（调色板 tRNS）→ 白底 */
            }
        }
        drow[x] = (a < 128) ? (uint16_t)0xFFFF
                            : (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    return true;
}

/** PNG：zlib 流式逐行解码 + unfilter + 行内最近邻抽样（峰值内存 ≈ 2 行扫描线）。 */
static esp_err_t epub_decode_png(const uint8_t *src, size_t src_len, int max_w, int max_h) {
    /* 文件签名 + IHDR */
    if (src_len < 33 || src[0] != 0x89 || src[1] != 'P' || src[2] != 'N' || src[3] != 'G' ||
        memcmp(&src[12], "IHDR", 4) != 0) {
        return ESP_FAIL;
    }
    const uint32_t w = ((uint32_t)src[16] << 24) | ((uint32_t)src[17] << 16) |
                       ((uint32_t)src[18] << 8) | src[19];
    const uint32_t h = ((uint32_t)src[20] << 24) | ((uint32_t)src[21] << 16) |
                       ((uint32_t)src[22] << 8) | src[23];
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
           ((h + (uint32_t)step - 1) / (uint32_t)step) > (uint32_t)max_h) {
        step++;
        if (step > 64) {
            step = 64;
            break;
        }
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
    bool failed = !zinit;
    size_t row_filled = 0;
    uint32_t row = 0;

    size_t p = 8; /* 跳过签名 */
    while (!failed && p + 8 <= src_len) {
        const uint32_t clen = ((uint32_t)src[p] << 24) | ((uint32_t)src[p + 1] << 16) |
                              ((uint32_t)src[p + 2] << 8) | src[p + 3];
        const char *ctype = (const char *)&src[p + 4];
        const size_t cdata = p + 8;
        if (cdata + clen + 4 > src_len) {
            failed = true;
            break;
        }
        if (memcmp(ctype, "IDAT", 4) == 0 && row < h) {
            zs.next_in = (Bytef *)&src[cdata];
            zs.avail_in = (uInt)clen;
            while (!failed && zs.avail_in > 0) {
                uint8_t *cur = &rows[(row % 2) * raw_row];
                zs.next_out = &cur[row_filled];
                zs.avail_out = (uInt)(raw_row - row_filled);
                const int zr = inflate(&zs, Z_NO_FLUSH);
                row_filled = raw_row - zs.avail_out;
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
                    if (row >= h) {
                        break;
                    }
                }
                if (zr == Z_STREAM_END) {
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
            break;
        }
        p = cdata + clen + 4; /* 数据 + CRC */
    }
    if (zinit) {
        inflateEnd(&zs);
    }
    heap_caps_free(rows);
    if (failed || row < h) {
        ESP_LOGW(TAG, "epub: png decode failed (row %u/%u)", (unsigned)row, (unsigned)h);
        heap_caps_free(dst);
        return ESP_FAIL;
    }
    s_epub.img_buf = (uint8_t *)dst;
    s_epub.img_dsc.header.w = dw;
    s_epub.img_dsc.header.h = dh;
    return ESP_OK;
}

#ifndef ESPAPERPLAY_READER_EPUB_HOST
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
static esp_err_t epub_decode_jpeg(const uint8_t *src, size_t src_len, int max_w, int max_h) {
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
    if (sw <= 0 || sh <= 0) {
        heap_caps_free(work);
        return ESP_FAIL;
    }
    int step = 1;
    while ((sw + step - 1) / step > max_w || (sh + step - 1) / step > max_h) {
        step <<= 1;
        if (step > 8) {
            step = 8;
            break;
        }
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
    s_epub.img_buf = (uint8_t *)ctx.dst;
    s_epub.img_dsc.header.w = dw;
    s_epub.img_dsc.header.h = dh;
    return ESP_OK;
}

#endif /* ESPAPERPLAY_READER_EPUB_HOST */

/* ====================================================================
 * 对外 API
 * ==================================================================== */

static void epub_image_cache_free(void);

static void epub_chapter_free(void) {
    epub_image_cache_free(); /* 图片 id 为章节内序号：换章即失效 */
    if (s_epub.ch_text != NULL) {
        heap_caps_free(s_epub.ch_text);
        s_epub.ch_text = NULL;
    }
    if (s_epub.ch_blocks != NULL) {
        heap_caps_free(s_epub.ch_blocks);
        s_epub.ch_blocks = NULL;
    }
    if (s_epub.ch_images != NULL) {
        heap_caps_free(s_epub.ch_images);
        s_epub.ch_images = NULL;
    }
    s_epub.ch_block_cnt = 0;
    s_epub.ch_image_cnt = 0;
    s_epub.ch_text_len = 0;
    s_epub.ch_title[0] = '\0';
    s_epub.cur_ch = -1;
}

static void epub_image_cache_free(void) {
    if (s_epub.img_buf != NULL) {
        heap_caps_free(s_epub.img_buf);
        s_epub.img_buf = NULL;
    }
    s_epub.img_cached_id = -1;
}

bool espaperplay_reader_is_epub(const char *path) {
    if (path == NULL) {
        return false;
    }
    const char *dot = strrchr(path, '.');
    return dot != NULL && strcasecmp(dot, ".epub") == 0;
}

#ifndef ESPAPERPLAY_READER_EPUB_HOST

/* ---------------- 预取 worker（内部 RAM 栈，优先级低于 LVGL） ---------------- */

#define EPUB_WORKER_STACK 6144 /* CSS 实例已在堆上，栈峰 ~4KB */
#define EPUB_WORKER_PRIO 4

static SemaphoreHandle_t s_req_sem;    /* 请求通知 */
static SemaphoreHandle_t s_slot_mutex; /* 就绪槽 + 请求字段互斥 */
static TaskHandle_t s_worker_task;
static int s_req_ch = -1;      /* 待产出章节（-1=无） */
static int s_loading_ch = -1;  /* worker 正在产出的章节 */
static epub_packet_t s_ready;  /* 就绪槽（chapter=-1 空） */

/* 分页缓存落盘任务（单槽；数组已堆复制，写完释放） */
typedef struct {
    int chapter;
    uint32_t font_key;
    int cnt;
    uint32_t *blocks;
    uint16_t *lines;
} epub_pagen_job_t;
static epub_pagen_job_t s_pagen_job;
static volatile bool s_pagen_pending = false;

/** worker 主循环：等待请求 → 产出（缓存优先）→ 放入就绪槽。 */

static void epub_worker_task(void *arg);

/* worker 状态：0=未创建 1=运行 2=不可用（创建失败，本次开书不再重试） */
static uint8_t s_worker_state;

/** worker 是否可用（不可用则同步降级）。 */
static bool epub_worker_available(void) { return s_worker_state == 1; }

/** 确保 worker 与信号量已创建（开书时调用；失败降级且不刷日志）。 */
static void epub_worker_ensure(void) {
    if (s_slot_mutex == NULL) {
        s_slot_mutex = xSemaphoreCreateMutex();
    }
    if (s_req_sem == NULL) {
        s_req_sem = xSemaphoreCreateBinary();
    }
    if (s_slot_mutex == NULL || s_req_sem == NULL) {
        return;
    }
    if (s_worker_state != 0) {
        return;
    }
    /* PSRAM 栈优先（内部 RAM 紧张：字形位图 / DMA 缓冲都在内部堆，能省则省；
     * SDMMC 不占 Flash 总线，PSRAM 栈安全），失败退内部 RAM 小栈，仍失败
     * 则同步降级（缓存读 + 内联解析） */
    if (xTaskCreateWithCaps(epub_worker_task, "epub_ld", 12288, NULL, EPUB_WORKER_PRIO,
                            &s_worker_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        s_worker_state = 1;
        return;
    }
    if (xTaskCreate(epub_worker_task, "epub_ld", EPUB_WORKER_STACK, NULL, EPUB_WORKER_PRIO,
                    &s_worker_task) == pdPASS) {
        s_worker_state = 1;
        ESP_LOGI(TAG, "epub: worker on internal stack (PSRAM alloc failed)");
        return;
    }
    s_worker_state = 2;
    s_worker_task = NULL;
    ESP_LOGW(TAG, "epub: prefetch worker unavailable, fallback to sync loading");
}

int espaperplay_reader_epub_pagen_load(int chapter, uint32_t font_key, uint32_t *blocks,
                                       uint16_t *lines, int max_cnt) {
    if (!s_epub.open) {
        return -1;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%08x.ch%03d.f%08x.pag", EPUB_CACHE_DIR,
             (unsigned)s_epub.token, chapter, (unsigned)font_key);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    uint32_t hdr[6] = {0};
    int cnt = -1;
    if (fread(hdr, sizeof(uint32_t), 6, f) == 6 && hdr[0] == EPUB_CACHE_MAGIC + 1u &&
        hdr[1] == EPUB_PAGEN_VER && hdr[2] == s_epub.token && hdr[3] == font_key &&
        hdr[4] == (uint32_t)chapter && hdr[5] <= (uint32_t)max_cnt && hdr[5] > 0) {
        const int n = (int)hdr[5];
        if (fread(blocks, sizeof(uint32_t), (size_t)n, f) == (size_t)n &&
            fread(lines, sizeof(uint16_t), (size_t)n, f) == (size_t)n) {
            cnt = n;
        }
    }
    fclose(f);
    if (cnt < 0) {
        remove(path); /* 损坏：删除待重建 */
    }
    return cnt;
}

void espaperplay_reader_epub_pagen_save_async(int chapter, uint32_t font_key, int cnt,
                                              const uint32_t *blocks, const uint16_t *lines) {
    if (!s_epub.open || cnt <= 0 || s_slot_mutex == NULL || s_req_sem == NULL) {
        return;
    }
    epub_worker_ensure();
    if (!epub_worker_available()) {
        return; /* 无 worker：放弃缓存（下次重算） */
    }
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    const bool busy = s_pagen_pending;
    xSemaphoreGive(s_slot_mutex);
    if (busy) {
        return; /* 上一任务还没写完：放弃（罕见） */
    }
    uint32_t *bcpy = heap_caps_malloc((size_t)cnt * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *lcpy = heap_caps_malloc((size_t)cnt * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bcpy == NULL || lcpy == NULL) {
        heap_caps_free(bcpy);
        heap_caps_free(lcpy);
        return;
    }
    memcpy(bcpy, blocks, (size_t)cnt * sizeof(uint32_t));
    memcpy(lcpy, lines, (size_t)cnt * sizeof(uint16_t));
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    s_pagen_job.chapter = chapter;
    s_pagen_job.font_key = font_key;
    s_pagen_job.cnt = cnt;
    s_pagen_job.blocks = bcpy;
    s_pagen_job.lines = lcpy;
    s_pagen_pending = true;
    xSemaphoreGive(s_slot_mutex);
    xSemaphoreGive(s_req_sem);
}

/** 预取章节（异步；重复 / 已就绪请求忽略；worker 不可用时静默跳过）。 */
static void epub_prefetch(int idx) {
    if (!s_epub.open || idx < 0 || idx >= s_epub.spine_cnt) {
        return;
    }
    if (!epub_worker_available()) {
        return;
    }
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    const bool need = (s_ready.chapter != idx) && (s_loading_ch != idx) && (s_req_ch != idx);
    if (need) {
        s_req_ch = idx;
    }
    xSemaphoreGive(s_slot_mutex);
    if (need) {
        xSemaphoreGive(s_req_sem);
    }
}

/** 从就绪槽取出章节包（匹配 token+idx；命中则移出并返回 true）。 */
static bool epub_steal_ready(int idx, epub_packet_t *out) {
    if (s_slot_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    const bool hit = s_epub.open && s_ready.chapter == idx && s_ready.token == s_epub.token;
    if (hit) {
        *out = s_ready;
        memset(&s_ready, 0, sizeof(s_ready));
        s_ready.chapter = -1;
    }
    xSemaphoreGive(s_slot_mutex);
    return hit;
}

#endif /* !ESPAPERPLAY_READER_EPUB_HOST */


esp_err_t espaperplay_reader_epub_open(const char *abs_path) {
    if (abs_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    espaperplay_reader_epub_close();
    memset(&s_epub.zip, 0, sizeof(s_epub.zip));
    s_epub.zip.fp = fopen(abs_path, "rb");
    if (s_epub.zip.fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = epub_zip_parse(&s_epub.zip);
    if (err != ESP_OK) {
        epub_zip_close(&s_epub.zip);
        return err;
    }

    /* container.xml → OPF 路径 */
    int zi = epub_zip_find(&s_epub.zip, "META-INF/container.xml");
    if (zi < 0) {
        epub_zip_close(&s_epub.zip);
        ESP_LOGW(TAG, "epub: container.xml missing");
        return ESP_ERR_NOT_SUPPORTED;
    }
    char *container = NULL;
    err = epub_zip_extract(&s_epub.zip, zi, &container, NULL);
    if (err != ESP_OK) {
        epub_zip_close(&s_epub.zip);
        return err;
    }
    char opf_path[600];
    const char *fp_attr = strcasestr(container, "full-path");
    bool ok = false;
    if (fp_attr != NULL) {
        const char *qe = strchr(fp_attr, '>');
        ok = qe != NULL && xml_attr(fp_attr, qe, "full-path", opf_path, sizeof(opf_path));
    }
    heap_caps_free(container);
    if (!ok) {
        epub_zip_close(&s_epub.zip);
        ESP_LOGW(TAG, "epub: OPF path not found in container.xml");
        return ESP_ERR_NOT_SUPPORTED;
    }

    zi = epub_zip_find(&s_epub.zip, opf_path);
    if (zi < 0) {
        epub_zip_close(&s_epub.zip);
        ESP_LOGW(TAG, "epub: OPF missing: %s", opf_path);
        return ESP_ERR_NOT_SUPPORTED;
    }
    path_dir(opf_path, s_epub.opf_dir, sizeof(s_epub.opf_dir));
    char *opf = NULL;
    err = epub_zip_extract(&s_epub.zip, zi, &opf, NULL);
    if (err != ESP_OK) {
        epub_zip_close(&s_epub.zip);
        return err;
    }
    err = epub_parse_opf(opf);
    heap_caps_free(opf);
    if (err != ESP_OK) {
        epub_zip_close(&s_epub.zip);
        ESP_LOGW(TAG, "epub: OPF parse failed");
        return err;
    }

    /* 书指纹：路径哈希 ^ mtime ^ size（缓存名与预取槽校验；书变更自动失效） */
    struct stat st;
    uint32_t token = 1073741827u;
    const char *tp_ = abs_path;
    while (*tp_ != '\0') {
        token = (token ^ (uint32_t)(unsigned char)*tp_++) * 16777619u; /* FNV-1a */
    }
    if (stat(abs_path, &st) == 0) {
        token ^= (uint32_t)st.st_mtime ^ (uint32_t)st.st_size;
    }
    s_epub.token = token;
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    s_worker_state = 0; /* 换书重试创建（上次失败可能是内存瞬时不足） */
    epub_worker_ensure();
    if (s_slot_mutex != NULL) {
        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        epub_packet_free(&s_ready);
        s_req_ch = -1;
        s_loading_ch = -1;
        xSemaphoreGive(s_slot_mutex);
    }
#endif
    s_epub.open = true;
    s_epub.cur_ch = -1;
    s_epub.img_cached_id = -1;
    ESP_LOGI(TAG, "epub: opened, title=\"%s\", %d chapter(s), %d manifest item(s)", s_epub.title,
             s_epub.spine_cnt, s_epub.item_cnt);
    return ESP_OK;
}

void espaperplay_reader_epub_close(void) {
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    if (s_slot_mutex != NULL) {
        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        epub_packet_free(&s_ready);
        s_req_ch = -1;
        s_loading_ch = -1;
        xSemaphoreGive(s_slot_mutex);
    }
#endif
    epub_chapter_free();
    epub_image_cache_free();
    if (s_epub.items != NULL) {
        for (int i = 0; i < s_epub.item_cnt; i++) {
            heap_caps_free(s_epub.items[i].zip_path);
        }
        heap_caps_free(s_epub.items);
        s_epub.items = NULL;
    }
    s_epub.item_cnt = 0;
    if (s_epub.spine != NULL) {
        heap_caps_free(s_epub.spine);
        s_epub.spine = NULL;
    }
    s_epub.spine_cnt = 0;
    epub_zip_close(&s_epub.zip);
    s_epub.open = false;
    s_epub.title[0] = '\0';
}

bool espaperplay_reader_epub_is_open(void) { return s_epub.open; }

const char *espaperplay_reader_epub_title(void) { return s_epub.title; }

int espaperplay_reader_epub_chapter_count(void) { return s_epub.open ? s_epub.spine_cnt : 0; }

#ifndef ESPAPERPLAY_READER_EPUB_HOST

/* ---------------- SD 解析缓存 ---------------- */

#define EPUB_CACHE_VER 1u

/** 缓存路径：书指纹 + 章节号（指纹含 mtime/size，书变更自动换名失效）。 */
static void epub_cache_path(char *buf, size_t n, uint32_t token, int idx) {
    snprintf(buf, n, "%s/%08x.ch%03d", EPUB_CACHE_DIR, (unsigned)token, idx);
}

/** 读缓存到包（命中返回 true；worker / LVGL 均可调用，只读不写）。 */
static bool epub_cache_read(epub_packet_t *pkt, uint32_t token, int idx) {
    char path[96];
    epub_cache_path(path, sizeof(path), token, idx);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    uint32_t hdr[6] = {0}; /* magic, ver, token, chapter, text_len, block_cnt */
    bool ok = false;
    do {
        if (fread(hdr, sizeof(uint32_t), 6, f) != 6) {
            break;
        }
        int32_t cnts[2] = {0}; /* block_cnt, image_cnt */
        if (fread(cnts, sizeof(int32_t), 2, f) != 2) {
            break;
        }
        if (hdr[0] != EPUB_CACHE_MAGIC || hdr[1] != EPUB_CACHE_VER || hdr[3] != (uint32_t)idx ||
            hdr[2] != token) {
            break;
        }
        if (cnts[0] < 0 || cnts[0] > 20000 || cnts[1] < 0 || cnts[1] > 256 ||
            hdr[4] == 0 || hdr[4] > ESPAPERPLAY_EPUB_MAX_ENTRY_BYTES) {
            break;
        }
        memset(pkt, 0, sizeof(*pkt));
        pkt->token = token;
        pkt->chapter = idx;
        if (fread(pkt->title, 1, sizeof(pkt->title), f) != sizeof(pkt->title)) {
            break;
        }
        pkt->title[sizeof(pkt->title) - 1] = '\0';
        pkt->block_cnt = cnts[0];
        pkt->image_cnt = cnts[1];
        pkt->text_len = hdr[4];
        bool bad = false;
        if (pkt->block_cnt > 0) {
            pkt->blocks = heap_caps_malloc((size_t)pkt->block_cnt * sizeof(*pkt->blocks),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pkt->blocks == NULL ||
                fread(pkt->blocks, sizeof(*pkt->blocks), (size_t)pkt->block_cnt, f) !=
                    (size_t)pkt->block_cnt) {
                bad = true;
            }
        }
        if (!bad && pkt->image_cnt > 0) {
            pkt->images = heap_caps_malloc((size_t)pkt->image_cnt * sizeof(int),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pkt->images == NULL ||
                fread(pkt->images, sizeof(int), (size_t)pkt->image_cnt, f) !=
                    (size_t)pkt->image_cnt) {
                bad = true;
            }
        }
        if (!bad) {
            pkt->text = heap_caps_malloc(pkt->text_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pkt->text == NULL ||
                fread(pkt->text, 1, pkt->text_len, f) != pkt->text_len) {
                bad = true;
            } else {
                pkt->text[pkt->text_len] = '\0';
            }
        }
        if (bad) {
            epub_packet_free(pkt);
            break;
        }
        ok = true;
    } while (false);
    fclose(f);
    if (!ok) {
        remove(path); /* 损坏缓存：删除待重建 */
    }
    return ok;
}

/** 写缓存（仅 worker 调用：SD 写不进 LVGL 线程）。 */
static void epub_cache_write(const epub_packet_t *pkt) {
    /* 目录惰性创建 */
    mkdir(ESPAPERPLAY_SYSTEM_SD_DIR, 0755);
    mkdir(ESPAPERPLAY_SYSTEM_SD_DIR "/cache", 0755);
    mkdir(EPUB_CACHE_DIR, 0755);
    char path[96];
    epub_cache_path(path, sizeof(path), pkt->token, pkt->chapter);
    char tmp[104];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return;
    }
    const uint32_t hdr[6] = {EPUB_CACHE_MAGIC,
                             EPUB_CACHE_VER,
                             pkt->token,
                             (uint32_t)pkt->chapter,
                             (uint32_t)pkt->text_len,
                             0};
    const int32_t cnts[2] = {pkt->block_cnt, pkt->image_cnt};
    bool ok = fwrite(hdr, sizeof(uint32_t), 6, f) == 6;
    ok = ok && fwrite(cnts, sizeof(int32_t), 2, f) == 2;
    ok = ok && fwrite(pkt->title, 1, sizeof(pkt->title), f) == sizeof(pkt->title);
    if (ok && pkt->block_cnt > 0) {
        ok = fwrite(pkt->blocks, sizeof(*pkt->blocks), (size_t)pkt->block_cnt, f) ==
             (size_t)pkt->block_cnt;
    }
    if (ok && pkt->image_cnt > 0) {
        ok = fwrite(pkt->images, sizeof(int), (size_t)pkt->image_cnt, f) ==
             (size_t)pkt->image_cnt;
    }
    ok = ok && fwrite(pkt->text, 1, pkt->text_len, f) == pkt->text_len;
    fclose(f);
    if (ok) {
        rename(tmp, path); /* 原子替换，避免半写文件被读到 */
    } else {
        remove(tmp);
    }
}

#endif /* !ESPAPERPLAY_READER_EPUB_HOST */

/**
 * 产出章节包：缓存命中直接返回；否则 zip 解压 + CSS 解析 + XHTML 块化
 * （持 zip 互斥，防与 epub_close/图片解码竞态）。安装由调用方完成。
 */
static esp_err_t epub_produce_packet(uint32_t token, int idx, bool use_cache, bool write_cache,
                                     epub_packet_t *out) {
    memset(out, 0, sizeof(*out));
    out->chapter = -1;

#ifndef ESPAPERPLAY_READER_EPUB_HOST
    if (use_cache && epub_cache_read(out, token, idx)) {
        return ESP_OK; /* 缓存命中：无需 zip */
    }
#endif

    epub_zip_lock();
    do {
        if (!s_epub.open || s_epub.token != token || idx < 0 || idx >= s_epub.spine_cnt) {
            break;
        }
        const int item_idx = s_epub.spine[idx];
        const char *zip_path = s_epub.items[item_idx].zip_path;
        const int zi = epub_zip_find(&s_epub.zip, zip_path);
        if (zi < 0) {
            break;
        }
        char *html = NULL;
        esp_err_t err = epub_zip_extract_unlocked(&s_epub.zip, zi, &html, NULL);
        if (err != ESP_OK) {
            break;
        }

        /* 章节标题：XHTML <title>…</title>（实体反转义） */
        char *tp = strcasestr(html, "<title");
        if (tp != NULL) {
            char *ts = strchr(tp, '>');
            char *te = ts != NULL ? strcasestr(ts, "</title") : NULL;
            if (ts != NULL && te != NULL && te > ts + 1 &&
                (size_t)(te - ts - 1) < sizeof(out->title)) {
                memcpy(out->title, ts + 1, (size_t)(te - ts - 1));
                out->title[te - ts - 1] = '\0';
                char *w = out->title;
                for (const char *r = out->title; *r != '\0';) {
                    if (r[0] == '&') {
                        if (strncmp(r, "&amp;", 5) == 0) {
                            *w++ = '&';
                            r += 5;
                            continue;
                        }
                        if (strncmp(r, "&lt;", 4) == 0) {
                            *w++ = '<';
                            r += 4;
                            continue;
                        }
                        if (strncmp(r, "&gt;", 4) == 0) {
                            *w++ = '>';
                            r += 4;
                            continue;
                        }
                    }
                    *w++ = *r++;
                }
                *w = '\0';
            }
        }

        /* 样式表：首个 <link rel=stylesheet>（每次产出本地解析，~几 ms；
         * 实例 ~3.5KB 放堆上，避免撑爆产出线程栈） */
        epub_css_t *css = heap_caps_malloc(sizeof(epub_css_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (css == NULL) {
            heap_caps_free(html);
            break;
        }
        epub_css_init(css);
        {
            char base_dir[300];
            path_dir(zip_path, base_dir, sizeof(base_dir));
            const char *lp = html;
            while ((lp = strcasestr(lp, "<link")) != NULL) {
                const char *te = strchr(lp, '>');
                if (te == NULL) {
                    break;
                }
                const char *xe = te[-1] == '/' ? te - 1 : te;
                char href[600];
                char rel[32];
                if (xml_attr(lp, xe, "href", href, sizeof(href)) &&
                    (!xml_attr(lp, xe, "rel", rel, sizeof(rel)) ||
                     strcasestr(rel, "stylesheet") != NULL)) {
                    url_decode(href);
                    char zp[600];
                    if (path_resolve(base_dir, href, zp, sizeof(zp))) {
                        const int czi = epub_zip_find(&s_epub.zip, zp);
                        if (czi >= 0 && s_epub.zip.ents[czi].usize <= 64 * 1024) {
                            char *csst = NULL;
                            if (epub_zip_extract_unlocked(&s_epub.zip, czi, &csst, NULL) ==
                                ESP_OK) {
                                epub_css_parse(css, csst);
                                heap_caps_free(csst);
                            }
                        }
                    }
                    break; /* 只取首个样式表 */
                }
                lp = te + 1;
            }
        }

        err = epub_parse_xhtml(html, zip_path, css, out);
        heap_caps_free(css);
        heap_caps_free(html);
        if (err != ESP_OK) {
            epub_packet_free(out);
            break;
        }
        out->token = token;
        out->chapter = idx;

#ifndef ESPAPERPLAY_READER_EPUB_HOST
        if (write_cache) {
            epub_cache_write(out); /* 仅 worker 走到此处 */
        }
#endif
        epub_zip_unlock();
        return ESP_OK;
    } while (false);
    epub_zip_unlock();
    return ESP_FAIL;
}

/** 包安装到 live 状态（指针所有权移交；调用方保证 token/章节有效）。 */
static void epub_install_packet(epub_packet_t *pkt) {
    epub_chapter_free(); /* 含图片缓存失效（图片 id 为章节内序号） */
    s_epub.ch_text = pkt->text;
    s_epub.ch_text_len = pkt->text_len;
    s_epub.ch_blocks = pkt->blocks;
    s_epub.ch_block_cnt = pkt->block_cnt;
    s_epub.ch_images = pkt->images;
    s_epub.ch_image_cnt = pkt->image_cnt;
    strlcpy(s_epub.ch_title, pkt->title, sizeof(s_epub.ch_title));
    s_epub.cur_ch = pkt->chapter;
    /* pkt 结构体本身在调用方栈上，不释放 */
}

#ifndef ESPAPERPLAY_READER_EPUB_HOST

/** worker 主循环：等待请求 → 产出（缓存优先）→ 放入就绪槽。 */
static void epub_worker_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_req_sem, portMAX_DELAY);

        /* 分页缓存落盘（优先：纯写操作，快） */
        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        const bool pagen = s_pagen_pending;
        xSemaphoreGive(s_slot_mutex);
        if (pagen) {
            xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
            const int pch = s_pagen_job.chapter;
            const uint32_t pfk = s_pagen_job.font_key;
            const int pcnt = s_pagen_job.cnt;
            uint32_t *pblk = s_pagen_job.blocks;
            uint16_t *pln = s_pagen_job.lines;
            s_pagen_pending = false;
            xSemaphoreGive(s_slot_mutex);
            if (pblk != NULL && pln != NULL && pcnt > 0) {
                char ppath[96];
                snprintf(ppath, sizeof(ppath), "%s/%08x.ch%03d.f%08x.pag", EPUB_CACHE_DIR,
                         (unsigned)s_epub.token, pch, (unsigned)pfk);
                char ptmp[104];
                snprintf(ptmp, sizeof(ptmp), "%s.tmp", ppath);
                FILE *pf = fopen(ptmp, "wb");
                if (pf != NULL) {
                    const uint32_t hdr[6] = {EPUB_CACHE_MAGIC + 1u, EPUB_PAGEN_VER,
                                             (unsigned)s_epub.token, pfk, (uint32_t)pch,
                                             (uint32_t)pcnt};
                    bool ok = fwrite(hdr, sizeof(uint32_t), 6, pf) == 6;
                    ok = ok && fwrite(pblk, sizeof(uint32_t), (size_t)pcnt, pf) == (size_t)pcnt;
                    ok = ok && fwrite(pln, sizeof(uint16_t), (size_t)pcnt, pf) == (size_t)pcnt;
                    fclose(pf);
                    if (ok) {
                        rename(ptmp, ppath);
                    } else {
                        remove(ptmp);
                    }
                }
            }
            heap_caps_free(pblk);
            heap_caps_free(pln);
        }

        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        const int idx = s_req_ch;
        const uint32_t token = s_epub.token;
        const bool open = s_epub.open;
        s_req_ch = -1;
        s_loading_ch = open ? idx : -1;
        xSemaphoreGive(s_slot_mutex);

        if (!open || idx < 0) {
            xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
            s_loading_ch = -1;
            xSemaphoreGive(s_slot_mutex);
            continue;
        }
        epub_packet_t pkt;
        const esp_err_t err = epub_produce_packet(token, idx, true, true, &pkt);
        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        s_loading_ch = -1;
        if (err == ESP_OK && s_epub.open && s_epub.token == token) {
            epub_packet_free(&s_ready);
            s_ready = pkt; /* 移交所有权 */
        } else {
            epub_packet_free(&pkt);
        }
        xSemaphoreGive(s_slot_mutex);
    }
}


#endif /* !ESPAPERPLAY_READER_EPUB_HOST */

esp_err_t espaperplay_reader_epub_load_chapter(int idx) {
    if (!s_epub.open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (idx < 0 || idx >= s_epub.spine_cnt) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_epub.cur_ch == idx) {
        return ESP_OK;
    }

    epub_packet_t pkt;
    esp_err_t err;
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    /* 1) 就绪槽（worker 预取结果，零解析开销） */
    if (epub_steal_ready(idx, &pkt)) {
        epub_install_packet(&pkt);
        ESP_LOGI(TAG, "epub: chapter %d adopted from prefetch (%u blocks)", idx,
                 (unsigned)s_epub.ch_block_cnt);
        epub_prefetch(idx + 1);
        return ESP_OK;
    }
#endif
    /* 2) 缓存 / 3) 内联解析（同步兜底；缓存写由 worker 预取兜底） */
    err = epub_produce_packet(s_epub.token, idx, true, false, &pkt);
    if (err != ESP_OK) {
        return err;
    }
    epub_install_packet(&pkt);
    ESP_LOGI(TAG, "epub: chapter %d loaded (%u blocks, %u image(s))", idx,
             (unsigned)s_epub.ch_block_cnt, (unsigned)s_epub.ch_image_cnt);
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    epub_prefetch(idx + 1);
#endif
    return ESP_OK;
}

#ifndef ESPAPERPLAY_READER_EPUB_HOST

bool espaperplay_reader_epub_poll_chapter(int idx) {
    if (!s_epub.open || idx < 0 || idx >= s_epub.spine_cnt) {
        return false;
    }
    if (s_epub.cur_ch == idx) {
        return true;
    }
    epub_packet_t pkt;
    if (epub_steal_ready(idx, &pkt)) {
        epub_install_packet(&pkt);
        return true;
    }
    if (epub_worker_available()) {
        epub_prefetch(idx); /* 未就绪：异步请求，稍后再试 */
        return false;
    }
    /* 降级：同步装载（缓存读优先）；失败也返回 true 让调用方标记坏章
     * （返回 false 会让轮询方无限重试）——由 blocks==NULL 表达空章 */
    if (espaperplay_reader_epub_load_chapter(idx) == ESP_OK || s_epub.cur_ch == idx) {
        return true;
    }
    return true; /* 坏章：驻留为空，分页计 0 页 */
}

#endif /* !ESPAPERPLAY_READER_EPUB_HOST */

int espaperplay_reader_epub_chapter_current(void) { return s_epub.cur_ch; }

const char *espaperplay_reader_epub_chapter_title(void) { return s_epub.ch_title; }

const char *espaperplay_reader_epub_chapter_text(size_t *out_len) {
    if (out_len != NULL) {
        *out_len = s_epub.ch_text_len;
    }
    return s_epub.ch_text;
}

const espaperplay_reader_block_t *espaperplay_reader_epub_blocks(int *out_cnt) {
    if (out_cnt != NULL) {
        *out_cnt = s_epub.ch_block_cnt;
    }
    return s_epub.ch_blocks;
}

#ifdef ESPAPERPLAY_READER_EPUB_HOST
/* 主机测试：PNG 走真实流式解码器（zlib 可用）；JPEG 不编译 */
esp_err_t espaperplay_reader_epub_image(int img_id, int max_w, int max_h,
                                        const lv_image_dsc_t **out_dsc) {
    if (!s_epub.open || s_epub.cur_ch < 0 || out_dsc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (img_id < 0 || img_id >= s_epub.ch_image_cnt) {
        return ESP_ERR_INVALID_ARG;
    }
    char name[600];
    const int zi = s_epub.ch_images[img_id];
    if (!epub_zip_ent_name(&s_epub.zip, zi, name, sizeof(name))) {
        return ESP_ERR_NOT_FOUND;
    }
    char *data = NULL;
    size_t data_len = 0;
    if (epub_zip_extract(&s_epub.zip, zi, &data, &data_len) != ESP_OK) {
        return ESP_FAIL;
    }
    memset(&s_epub.img_dsc, 0, sizeof(s_epub.img_dsc));
    s_epub.img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    const esp_err_t err = epub_decode_png((const uint8_t *)data, data_len, max_w, max_h);
    heap_caps_free(data);
    if (err != ESP_OK) {
        return err;
    }
    s_epub.img_dsc.header.stride = s_epub.img_dsc.header.w * 2;
    s_epub.img_dsc.data_size = (uint32_t)(s_epub.img_dsc.header.w * s_epub.img_dsc.header.h * 2);
    s_epub.img_dsc.data = s_epub.img_buf;
    s_epub.img_cached_id = img_id;
    *out_dsc = &s_epub.img_dsc;
    return ESP_OK;
}
#else
esp_err_t espaperplay_reader_epub_image(int img_id, int max_w, int max_h,
                                        const lv_image_dsc_t **out_dsc) {
    if (!s_epub.open || s_epub.cur_ch < 0 || out_dsc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (img_id < 0 || img_id >= s_epub.ch_image_cnt) {
        return ESP_ERR_INVALID_ARG;
    }
    if (img_id == s_epub.img_cached_id && s_epub.img_buf != NULL) {
        *out_dsc = &s_epub.img_dsc;
        return ESP_OK;
    }
    epub_image_cache_free();

    char name[600];
    if (!epub_zip_ent_name(&s_epub.zip, s_epub.ch_images[img_id], name, sizeof(name))) {
        return ESP_ERR_NOT_FOUND;
    }
    const int zi = s_epub.ch_images[img_id];
    char *data = NULL;
    size_t data_len = 0;
    esp_err_t err = epub_zip_extract(&s_epub.zip, zi, &data, &data_len);
    if (err != ESP_OK) {
        return err;
    }

    memset(&s_epub.img_dsc, 0, sizeof(s_epub.img_dsc));
    s_epub.img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    const char *dot = strrchr(name, '.');
    if (dot != NULL && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)) {
        err = epub_decode_jpeg((const uint8_t *)data, data_len, max_w, max_h);
    } else if (dot != NULL && strcasecmp(dot, ".png") == 0) {
        err = epub_decode_png((const uint8_t *)data, data_len, max_w, max_h);
    } else if (data_len > 3 && (uint8_t)data[0] == 0xFF && (uint8_t)data[1] == 0xD8) {
        err = epub_decode_jpeg((const uint8_t *)data, data_len, max_w, max_h);
    } else if (data_len > 8 && (uint8_t)data[1] == 'P' && (uint8_t)data[2] == 'N' &&
               (uint8_t)data[3] == 'G') {
        err = epub_decode_png((const uint8_t *)data, data_len, max_w, max_h);
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    heap_caps_free(data);
    if (err != ESP_OK) {
        return err;
    }
    s_epub.img_dsc.header.stride = s_epub.img_dsc.header.w * 2;
    s_epub.img_dsc.data_size = (uint32_t)(s_epub.img_dsc.header.w * s_epub.img_dsc.header.h * 2);
    s_epub.img_dsc.data = s_epub.img_buf;
    s_epub.img_cached_id = img_id;
    *out_dsc = &s_epub.img_dsc;
    ESP_LOGI(TAG, "epub: image %d decoded (%dx%d)", img_id, s_epub.img_dsc.header.w,
             s_epub.img_dsc.header.h);
    return ESP_OK;
}
#endif
