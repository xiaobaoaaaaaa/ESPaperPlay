/* SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 * SPDX-License-Identifier: MIT */
#include "espaperplay_reader_epub.h"
#include "esp_log.h"
#include "espaperplay_config.h"
#include "espcache.h"
#include "parser/reader_image.h"
#include "parser/reader_package.h"
#include "parser/reader_xhtml.h"
#include <string.h>
#include <strings.h>
#ifndef ESPAPERPLAY_READER_EPUB_HOST
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

/* The session owns all mutable state. Parsers only receive explicit contexts.
 * Lock order: book -> slot. A worker never holds the slot while acquiring book.
 * A generation identifies a live session; the stable token identifies disk caches. */
#define EPUB_CACHE_DIR ESPAPERPLAY_SYSTEM_SD_DIR "/cache/reader"
#define EPUB_CACHE_MAGIC 0x43525045u
#define EPUB_ENGINE_VERSION 0x45505204u
static const char *TAG = "ESPaperPlay_READER";
typedef struct {
    uint32_t token;
    int chapter;
    char title[128];
    char *text;
    size_t text_len;
    espaperplay_reader_block_t *blocks;
    int block_cnt;
    int *images;
    int image_cnt;
    esp_err_t status;
} epub_packet_t;
static epub_zip_t s_zip;
static rp_package_t s_book;
static epub_packet_t s_live = {.chapter = -1};
static bool s_open;
static uint32_t s_token, s_generation;
static uint8_t *s_image;
static lv_image_dsc_t s_image_dsc;
static int s_image_id = -1, s_image_w, s_image_h;
#ifndef ESPAPERPLAY_READER_EPUB_HOST
static SemaphoreHandle_t s_book_lock, s_slot_lock, s_notify;
static TaskHandle_t s_worker;
static epub_packet_t s_ready = {.chapter = -1};
static int s_request = -1, s_loading = -1;
static uint32_t s_request_generation;
static void book_lock(void) {
    if (s_book_lock)
        xSemaphoreTakeRecursive(s_book_lock, portMAX_DELAY);
}
static void book_unlock(void) {
    if (s_book_lock)
        xSemaphoreGiveRecursive(s_book_lock);
}
#else
static void book_lock(void) {}
static void book_unlock(void) {}
#endif
static void epub_packet_free(epub_packet_t *p) {
    rp_free(p->text);
    rp_free(p->blocks);
    rp_free(p->images);
    memset(p, 0, sizeof(*p));
    p->chapter = -1;
}
static void image_free(void) {
    rp_free(s_image);
    s_image = NULL;
    s_image_id = -1;
    memset(&s_image_dsc, 0, sizeof(s_image_dsc));
}
#ifndef ESPAPERPLAY_READER_EPUB_HOST
#include "parser/reader_cache.inc"
#endif
static esp_err_t produce(int idx, bool write_cache, epub_packet_t *out) {
    memset(out, 0, sizeof(*out));
    out->chapter = -1;
    if (!s_open || idx < 0 || idx >= s_book.chapter_count)
        return ESP_ERR_INVALID_ARG;
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    if (epub_cache_read(out, s_token, idx))
        return ESP_OK;
#else
    (void)write_cache;
#endif
    rp_content_t content = {0};
    esp_err_t err = ESP_OK;
    const rp_chapter_t *first = &s_book.chapters[idx];
    const rp_chapter_t *last = idx + 1 < s_book.chapter_count ? &s_book.chapters[idx + 1] : NULL;
    int end_doc = last ? last->doc : s_book.spine_cnt;
    for (int d = first->doc;
         d < s_book.spine_cnt && (d < end_doc || (d == end_doc && last->offset)); d++) {
        const char *path = s_book.items[s_book.spine[d]].zip_path;
        char *html = NULL;
        size_t len;
        err = rp_zip_xml(&s_zip, epub_zip_find(&s_zip, path), &html, &len);
        if (err != ESP_OK)
            break;
        uint32_t start = d == first->doc ? first->offset : 0,
                 end = last && d == last->doc ? last->offset : UINT32_MAX;
        err = rp_content_document(&s_zip, path, html, len, start, end, &content);
        rp_free(html);
        if (err != ESP_OK)
            break;
    }
    if (err != ESP_OK) {
        rp_content_free(&content);
        return err;
    }
    out->token = s_token;
    out->chapter = idx;
    out->text = content.text;
    out->text_len = content.text_len;
    out->blocks = content.blocks;
    out->block_cnt = content.block_count;
    out->images = content.images;
    out->image_cnt = content.image_count;
    snprintf(out->title, sizeof(out->title), "%s", first->title[0] ? first->title : content.title);
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    if (write_cache)
        epub_cache_write(out);
#endif
    return ESP_OK;
}
static void install(epub_packet_t *p) {
    image_free();
    epub_packet_free(&s_live);
    s_live = *p;
    memset(p, 0, sizeof(*p));
    p->chapter = -1;
}
#ifndef ESPAPERPLAY_READER_EPUB_HOST
static void worker(void *unused) {
    (void)unused;
    for (;;) {
        xSemaphoreTake(s_notify, portMAX_DELAY);
        xSemaphoreTake(s_slot_lock, portMAX_DELAY);
        int idx = s_request;
        uint32_t generation = s_request_generation;
        s_request = -1;
        s_loading = idx;
        xSemaphoreGive(s_slot_lock);
        book_lock();
        if (idx >= 0 && s_open && generation == s_generation) {
            epub_packet_t result;
            esp_err_t err = produce(idx, true, &result);
            if (err != ESP_OK) {
                epub_packet_free(&result);
                result.chapter = idx;
                result.token = s_token;
                result.status = err;
                ESP_LOGW(TAG, "chapter %d parse failed: %s", idx, esp_err_to_name(err));
            }
            xSemaphoreTake(s_slot_lock, portMAX_DELAY);
            epub_packet_free(&s_ready);
            s_ready = result;
            s_loading = -1;
            xSemaphoreGive(s_slot_lock);
        } else {
            xSemaphoreTake(s_slot_lock, portMAX_DELAY);
            s_loading = -1;
            xSemaphoreGive(s_slot_lock);
        }
        book_unlock();
    }
}
static void worker_init(void) {
    if (s_worker)
        return;
    if (!s_slot_lock)
        s_slot_lock = xSemaphoreCreateMutex();
    if (!s_notify)
        s_notify = xSemaphoreCreateBinary();
    if (s_slot_lock && s_notify)
        xTaskCreateWithCaps(worker, "reader_parse", 24576, NULL, 4, &s_worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void prefetch(int idx) {
    if (!s_worker || idx < 0 || idx >= s_book.chapter_count)
        return;
    xSemaphoreTake(s_slot_lock, portMAX_DELAY);
    if (s_ready.chapter != idx && s_loading != idx) {
        s_request = idx;
        s_request_generation = s_generation;
        xSemaphoreGive(s_notify);
    }
    xSemaphoreGive(s_slot_lock);
}
static bool take_ready(int idx, epub_packet_t *out) {
    if (!s_slot_lock)
        return false;
    bool found = false;
    xSemaphoreTake(s_slot_lock, portMAX_DELAY);
    if (s_ready.chapter == idx && s_ready.token == s_token) {
        *out = s_ready;
        memset(&s_ready, 0, sizeof(s_ready));
        s_ready.chapter = -1;
        found = true;
    }
    xSemaphoreGive(s_slot_lock);
    return found;
}
#endif
bool espaperplay_reader_is_epub(const char *path) {
    const char *ext = path ? strrchr(path, '.') : NULL;
    return ext && !strcasecmp(ext, ".epub");
}
esp_err_t espaperplay_reader_epub_open(const char *path) {
    if (!path)
        return ESP_ERR_INVALID_ARG;
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    if (!s_book_lock)
        s_book_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_book_lock)
        return ESP_ERR_NO_MEM;
#endif
    espaperplay_reader_epub_close();
    book_lock();
    s_zip.fp = fopen(path, "rb");
    esp_err_t err = s_zip.fp ? epub_zip_parse(&s_zip) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK)
        err = rp_package_open(&s_zip, &s_book);
    if (err != ESP_OK) {
        rp_package_free(&s_book);
        rp_zip_close(&s_zip);
        book_unlock();
        return err;
    }
    s_token = espcache_file_token(path) ^ EPUB_ENGINE_VERSION;
    s_open = true;
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    worker_init();
#endif
    ESP_LOGI(TAG, "opened EPUB: %d chapters, %d spine documents, %d resources",
             s_book.chapter_count, s_book.spine_cnt, s_book.item_cnt);
    book_unlock();
    return ESP_OK;
}
void espaperplay_reader_epub_close(void) {
    book_lock();
    s_generation++;
    s_open = false;
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    if (s_slot_lock) {
        xSemaphoreTake(s_slot_lock, portMAX_DELAY);
        epub_packet_free(&s_ready);
        s_request = -1;
        xSemaphoreGive(s_slot_lock);
    }
#endif
    epub_packet_free(&s_live);
    image_free();
    rp_package_free(&s_book);
    rp_zip_close(&s_zip);
    book_unlock();
}
bool espaperplay_reader_epub_is_open(void) { return s_open; }
const char *espaperplay_reader_epub_title(void) { return s_book.title; }
uint32_t espaperplay_reader_epub_token(void) { return s_token; }
int espaperplay_reader_epub_chapter_count(void) { return s_open ? s_book.chapter_count : 0; }
bool espaperplay_reader_epub_toc_title(int idx, char *buf, size_t len) {
    if (!s_open || idx < 0 || idx >= s_book.chapter_count || !buf || !len)
        return false;
    snprintf(buf, len, "%s", s_book.chapters[idx].title);
    return buf[0] != 0;
}
esp_err_t espaperplay_reader_epub_load_chapter(int idx) {
    if (!s_open || idx < 0 || idx >= s_book.chapter_count)
        return ESP_ERR_INVALID_ARG;
    if (s_live.chapter == idx)
        return s_live.status;
    epub_packet_t packet;
    book_lock();
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    bool ready = take_ready(idx, &packet);
    esp_err_t err = ready ? packet.status : produce(idx, false, &packet);
#else
    esp_err_t err = produce(idx, false, &packet);
#endif
    if (err == ESP_OK)
        install(&packet);
    else
        epub_packet_free(&packet);
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    if (err == ESP_OK)
        prefetch(idx + 1);
#endif
    book_unlock();
    return err;
}
bool espaperplay_reader_epub_poll_chapter(int idx) {
    if (!s_open || idx < 0 || idx >= s_book.chapter_count)
        return false;
    if (s_live.chapter == idx)
        return true;
#ifndef ESPAPERPLAY_READER_EPUB_HOST
    epub_packet_t packet;
    if (take_ready(idx, &packet)) {
        install(&packet);
        return true;
    }
    if (s_worker) {
        prefetch(idx);
        return false;
    }
#endif
    esp_err_t err = espaperplay_reader_epub_load_chapter(idx);
    if (err != ESP_OK) {
        image_free();
        epub_packet_free(&s_live);
        s_live.chapter = idx;
        s_live.status = err;
    }
    return true;
}
int espaperplay_reader_epub_chapter_current(void) { return s_live.chapter; }
const char *espaperplay_reader_epub_chapter_title(void) { return s_live.title; }
const char *espaperplay_reader_epub_chapter_text(size_t *len) {
    if (len)
        *len = s_live.text_len;
    return s_live.text;
}
const espaperplay_reader_block_t *espaperplay_reader_epub_blocks(int *count) {
    if (count)
        *count = s_live.block_cnt;
    return s_live.blocks;
}
static void descriptor(lv_image_dsc_t *d, const epub_img_out_t *out) {
    memset(d, 0, sizeof(*d));
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = out->w;
    d->header.h = out->h;
    d->header.stride = out->w * 2;
    d->data_size = out->w * out->h * 2;
    d->data = out->buf;
}
static esp_err_t decode_entry(epub_zip_t *zip, int zi, int w, int h, epub_img_out_t *out) {
    char path[RP_PATH];
    if (!epub_zip_ent_name(zip, zi, path, sizeof(path)))
        return ESP_ERR_NOT_FOUND;
    char *data = NULL;
    size_t len;
    esp_err_t err = epub_zip_extract_unlocked(zip, zi, &data, &len);
    if (err == ESP_OK)
        err = rp_image_decode(path, (uint8_t *)data, len, w, h, out);
    rp_free(data);
    return err;
}
esp_err_t espaperplay_reader_epub_image(int id, int w, int h, const lv_image_dsc_t **out) {
    if (!s_open || !out || id < 0 || id >= s_live.image_cnt)
        return ESP_ERR_INVALID_ARG;
    if (s_image && s_image_id == id && s_image_w == w && s_image_h == h) {
        *out = &s_image_dsc;
        return ESP_OK;
    }
    image_free();
    epub_img_out_t decoded = {0};
    book_lock();
    esp_err_t err = decode_entry(&s_zip, s_live.images[id], w, h, &decoded);
    book_unlock();
    if (err != ESP_OK)
        return err;
    s_image = decoded.buf;
    s_image_id = id;
    s_image_w = w;
    s_image_h = h;
    descriptor(&s_image_dsc, &decoded);
    *out = &s_image_dsc;
    return ESP_OK;
}
esp_err_t espaperplay_reader_epub_probe_cover(const char *path, int w, int h, lv_image_dsc_t *d,
                                              uint8_t **buf) {
    if (!path || !d || !buf || w <= 0 || h <= 0)
        return ESP_ERR_INVALID_ARG;
    *buf = NULL;
    epub_zip_t zip = {.fp = fopen(path, "rb")};
    rp_package_t book = {0};
    if (!zip.fp)
        return ESP_ERR_NOT_FOUND;
    esp_err_t err = epub_zip_parse(&zip);
    if (err == ESP_OK)
        err = rp_package_metadata(&zip, &book);
    int cover = book.cover_entry;
    if (err == ESP_OK && cover < 0 && book.spine_cnt) {
        const char *doc = book.items[book.spine[0]].zip_path;
        char *html = NULL;
        size_t len;
        if (rp_zip_xml(&zip, epub_zip_find(&zip, doc), &html, &len) == ESP_OK) {
            rp_xml_t x;
            rp_xml_token_t t;
            rp_xml_init(&x, html, len);
            while (rp_xml_next(&x, &t) != RP_XML_EOF && t.kind != RP_XML_ERROR) {
                if (t.kind != RP_XML_START || (strcmp(t.name, "img") && strcmp(t.name, "image")))
                    continue;
                char href[RP_PATH], resolved[RP_PATH];
                if ((rp_xml_attr(&t, "src", href, sizeof(href)) ||
                     rp_xml_attr(&t, "href", href, sizeof(href))) &&
                    rp_uri(doc, href, resolved, NULL)) {
                    cover = epub_zip_find(&zip, resolved);
                    if (cover >= 0)
                        break;
                }
            }
            rp_free(html);
        }
    }
    if (err == ESP_OK && cover < 0)
        for (int i = 0; i < zip.ent_cnt; i++) {
            const char *name = zip.names + zip.ents[i].name_off, *base = strrchr(name, '/');
            base = base ? base + 1 : name;
            if (!strcasecmp(base, "cover.jpg") || !strcasecmp(base, "cover.jpeg") ||
                !strcasecmp(base, "cover.png")) {
                cover = i;
                break;
            }
        }
    epub_img_out_t decoded = {0};
    if (err == ESP_OK)
        err = cover >= 0 ? decode_entry(&zip, cover, w, h, &decoded) : ESP_ERR_NOT_FOUND;
    rp_package_free(&book);
    rp_zip_close(&zip);
    if (err != ESP_OK)
        return err;
    descriptor(d, &decoded);
    *buf = decoded.buf;
    return ESP_OK;
}
