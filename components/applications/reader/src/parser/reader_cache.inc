/* SPDX-License-Identifier: MIT
 * Private cache serialization, included by the EPUB session owner. */
#define EPUB_CACHE_VER 4u

/** 缓存路径：书指纹 + 章节号（指纹含 mtime/size，书变更自动换名失效）。 */
static void epub_cache_path(char *buf, size_t n, uint32_t token, int idx) {
    snprintf(buf, n, "%s/%08x.ch%03d", EPUB_CACHE_DIR, (unsigned)token, idx);
}

/** 读缓存回调：校验头并把章节包读入 PSRAM（损坏由模块统一删文件）。
 *  调用方在 pkt 中预填 token/chapter 作校验键。 */
static espcache_result_t epub_cache_read_fn(FILE *f, void *ud) {
    epub_packet_t *pkt = ud;
    const uint32_t token = pkt->token;
    const int idx = pkt->chapter;
    uint32_t hdr[6] = {0}; /* magic, ver, token, chapter, text_len, block_cnt */
    if (fread(hdr, sizeof(uint32_t), 6, f) != 6) {
        return ESPCACHE_CORRUPT;
    }
    int32_t cnts[2] = {0}; /* block_cnt, image_cnt */
    if (fread(cnts, sizeof(int32_t), 2, f) != 2) {
        return ESPCACHE_CORRUPT;
    }
    if (hdr[0] != EPUB_CACHE_MAGIC || hdr[1] != EPUB_CACHE_VER || hdr[3] != (uint32_t)idx ||
        hdr[2] != token) {
        return ESPCACHE_CORRUPT;
    }
    if (cnts[0] < 0 || cnts[0] > 20000 || cnts[1] < 0 || cnts[1] > 256 || hdr[4] > RP_MAX_TEXT ||
        hdr[5] > RP_MAX_SPANS) {
        return ESPCACHE_CORRUPT;
    }
    memset(pkt, 0, sizeof(*pkt));
    pkt->token = token;
    pkt->chapter = idx;
    if (fread(pkt->title, 1, sizeof(pkt->title), f) != sizeof(pkt->title)) {
        return ESPCACHE_CORRUPT;
    }
    pkt->title[sizeof(pkt->title) - 1] = '\0';
    pkt->block_cnt = cnts[0];
    pkt->image_cnt = cnts[1];
    pkt->span_cnt = hdr[5];
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
    if (!bad && pkt->span_cnt > 0) {
        pkt->spans = rp_alloc((size_t)pkt->span_cnt * sizeof(*pkt->spans));
        if (!pkt->spans ||
            fread(pkt->spans, sizeof(*pkt->spans), pkt->span_cnt, f) != (size_t)pkt->span_cnt)
            bad = true;
    }
    if (!bad && pkt->image_cnt > 0) {
        pkt->images = heap_caps_malloc((size_t)pkt->image_cnt * sizeof(int),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pkt->images == NULL ||
            fread(pkt->images, sizeof(int), (size_t)pkt->image_cnt, f) != (size_t)pkt->image_cnt) {
            bad = true;
        }
    }
    if (!bad) {
        pkt->text = heap_caps_malloc(pkt->text_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pkt->text == NULL || fread(pkt->text, 1, pkt->text_len, f) != pkt->text_len) {
            bad = true;
        } else {
            pkt->text[pkt->text_len] = '\0';
        }
    }
    for (int i=0; !bad && i<pkt->block_cnt; i++) {
        const espaperplay_reader_block_t *b=&pkt->blocks[i];
        bad = b->off > pkt->text_len || b->len > pkt->text_len - b->off || b->image < -1 ||
              b->image >= pkt->image_cnt || (b->scale && (b->scale < 50 || b->scale > 200)) ||
              b->line_height > 640 || b->layout > 3 || b->indent < -512 || b->indent > 512 ||
              b->before < 0 || b->before > 640 || b->after < 0 || b->after > 640;
    }
    for (int i = 0; !bad && i < pkt->span_cnt; i++) {
        const espaperplay_reader_span_t *s = &pkt->spans[i];
        bad = s->off > pkt->text_len || !s->len || s->len > pkt->text_len - s->off ||
              s->scale < 50 || s->scale > 200 ||
              (i && s->off < pkt->spans[i - 1].off + pkt->spans[i - 1].len);
    }
    for (int i=0; !bad && i<pkt->image_cnt; i++) bad=pkt->images[i]<0 || pkt->images[i]>=s_zip.ent_cnt;
    if (!bad && (memchr(pkt->text,0,pkt->text_len) || fgetc(f)!=EOF)) bad=true;
    if (bad) {
        epub_packet_free(pkt);
        return ESPCACHE_CORRUPT;
    }
    return ESPCACHE_OK;
}

/** 读缓存到包（命中返回 true；worker / LVGL 均可调用，只读不写）。 */
static bool epub_cache_read(epub_packet_t *pkt, uint32_t token, int idx) {
    char path[96];
    epub_cache_path(path, sizeof(path), token, idx);
    pkt->token = token;
    pkt->chapter = idx;
    return espcache_read(path, epub_cache_read_fn, pkt) == ESPCACHE_OK;
}

/** 写缓存回调：头 + 计数 + 标题 + blocks/images/text 载荷。 */
static bool epub_cache_write_fn(FILE *f, void *ud) {
    const epub_packet_t *pkt = ud;
    const uint32_t hdr[6] = {
        EPUB_CACHE_MAGIC,       EPUB_CACHE_VER,          pkt->token,
        (uint32_t)pkt->chapter, (uint32_t)pkt->text_len, (uint32_t)pkt->span_cnt};
    const int32_t cnts[2] = {pkt->block_cnt, pkt->image_cnt};
    bool ok = fwrite(hdr, sizeof(uint32_t), 6, f) == 6;
    ok = ok && fwrite(cnts, sizeof(int32_t), 2, f) == 2;
    ok = ok && fwrite(pkt->title, 1, sizeof(pkt->title), f) == sizeof(pkt->title);
    if (ok && pkt->block_cnt > 0) {
        ok = fwrite(pkt->blocks, sizeof(*pkt->blocks), (size_t)pkt->block_cnt, f) ==
             (size_t)pkt->block_cnt;
    }
    if (ok && pkt->span_cnt > 0)
        ok = fwrite(pkt->spans, sizeof(*pkt->spans), pkt->span_cnt, f) == (size_t)pkt->span_cnt;
    if (ok && pkt->image_cnt > 0) {
        ok = fwrite(pkt->images, sizeof(int), (size_t)pkt->image_cnt, f) ==
             (size_t)pkt->image_cnt;
    }
    ok = ok && fwrite(pkt->text, 1, pkt->text_len, f) == pkt->text_len;
    return ok;
}

/** 写缓存（仅 worker 调用：SD 写不进 LVGL 线程）。 */
static void epub_cache_write(const epub_packet_t *pkt) {
    char path[96];
    epub_cache_path(path, sizeof(path), pkt->token, pkt->chapter);
    espcache_write_atomic(path, epub_cache_write_fn, (void *)pkt);
}
