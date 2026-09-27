/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_reader.h"
#include "espcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "espaperplay_config.h"
#include "espaperplay_reader_epub.h"
#include "espaperplay_reader_history.h"
#include "espaperplay_reader_pagen.h"
#include "espaperplay_reader_txt.h"
#include "espaperplay_storage.h"
#include "parser/reader_text.h"

static const char *TAG = "ESPaperPlay_READER";

static rp_text_t s_txt;
static char s_txt_chapter_title[128];

static espaperplay_reader_fmt_t s_fmt = ESPAPERPLAY_READER_FMT_NONE;
static char s_path[256] = {0};   /*!< 当前文档绝对路径 */
static uint32_t s_txt_token = 0; /*!< TXT 书指纹（分页缓存键；EPUB 用 epub 模块的 token） */
static char s_title[128] = {0};
static bool s_open = false; /*!< 是否已打开 */

esp_err_t espaperplay_reader_init(void) {
    ESP_LOGI(TAG, "Reader framework init");
    (void)espaperplay_reader_history_init();
    return ESP_OK;
}

static void reader_close_internal(void) {
    rp_text_close(&s_txt);
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        espaperplay_reader_epub_close();
    }
    s_fmt = ESPAPERPLAY_READER_FMT_NONE;
    s_path[0] = '\0';
    s_title[0] = '\0';
    s_open = false;
}

esp_err_t espaperplay_reader_close(void) {
    reader_close_internal();
    ESP_LOGI(TAG, "reader closed");
    return ESP_OK;
}

bool espaperplay_reader_is_open(void) { return s_open; }

const char *espaperplay_reader_get_path(void) { return s_open ? s_path : NULL; }

/** TXT indexes raw offsets once; only the current bounded segment is decoded. */
static esp_err_t reader_open_txt(const char *abs_path) {
    reader_close_internal();
    esp_err_t err = rp_text_open(&s_txt, abs_path);
    if (err != ESP_OK)
        return err;
    s_fmt = ESPAPERPLAY_READER_FMT_TXT;
    const char *base = strrchr(abs_path, '/');
    base = base ? base + 1 : abs_path;
    strlcpy(s_title, base, sizeof(s_title));
    char *dot = strrchr(s_title, '.');
    if (dot)
        *dot = 0;
    s_txt_token = espcache_file_token(abs_path) ^ 0x54585402u;
    strlcpy(s_path, abs_path, sizeof(s_path));
    s_open = true;
    ESP_LOGI(TAG, "opened TXT: %d segments, encoding %d", s_txt.count, s_txt.encoding);
    return ESP_OK;
}

bool espaperplay_reader_is_txt_file(const char *path) { return espaperplay_reader_is_txt(path); }

bool espaperplay_reader_is_supported_file(const char *path) {
    return espaperplay_reader_is_txt_file(path) || espaperplay_reader_is_epub(path);
}

espaperplay_reader_fmt_t espaperplay_reader_get_fmt(void) {
    return s_open ? s_fmt : ESPAPERPLAY_READER_FMT_NONE;
}

const char *espaperplay_reader_get_title(void) { return s_open ? s_title : NULL; }

int espaperplay_reader_chapter_count(void) {
    if (!s_open) {
        return 0;
    }
    return s_fmt == ESPAPERPLAY_READER_FMT_EPUB ? espaperplay_reader_epub_chapter_count()
                                                : s_txt.count;
}

bool espaperplay_reader_toc_title(int idx, char *buf, size_t len) {
    if (!s_open || buf == NULL || len == 0) {
        return false;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        return espaperplay_reader_epub_toc_title(idx, buf, len);
    }
    if (idx < 0 || idx >= s_txt.count) {
        return false;
    }
    if (s_txt.count == 1)
        strlcpy(buf, s_title, len);
    else
        snprintf(buf, len, "第 %d / %d 节", idx + 1, s_txt.count);
    return buf[0] != '\0';
}

esp_err_t espaperplay_reader_load_chapter(int idx) {
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_TXT) {
        return rp_text_load(&s_txt, idx);
    }
    return espaperplay_reader_epub_load_chapter(idx);
}

int espaperplay_reader_chapter_current(void) {
    if (!s_open) {
        return -1;
    }
    return s_fmt == ESPAPERPLAY_READER_FMT_EPUB ? espaperplay_reader_epub_chapter_current()
                                                : s_txt.current;
}

bool espaperplay_reader_poll_chapter(int idx) {
    if (!s_open) {
        return false;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        return espaperplay_reader_epub_poll_chapter(idx);
    }
    if (idx < 0 || idx >= s_txt.count)
        return false;
    esp_err_t err = rp_text_load(&s_txt, idx);
    if (err != ESP_OK) {
        rp_free(s_txt.text);
        rp_free(s_txt.blocks);
        s_txt.text = NULL;
        s_txt.blocks = NULL;
        s_txt.text_len = 0;
        s_txt.block_count = 0;
        s_txt.current = idx;
        s_txt.status = err;
    }
    return true;
}

const char *espaperplay_reader_chapter_title(void) {
    if (!s_open) {
        return NULL;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        return espaperplay_reader_epub_chapter_title();
    }
    if (s_txt.count == 1)
        return s_title;
    snprintf(s_txt_chapter_title, sizeof(s_txt_chapter_title), "第 %d / %d 节", s_txt.current + 1,
             s_txt.count);
    return s_txt_chapter_title;
}

const char *espaperplay_reader_chapter_text(size_t *out_len) {
    if (!s_open) {
        return NULL;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        return espaperplay_reader_epub_chapter_text(out_len);
    }
    if (out_len != NULL) {
        *out_len = s_txt.text_len;
    }
    return s_txt.text;
}

const espaperplay_reader_block_t *espaperplay_reader_blocks(int *out_cnt) {
    if (!s_open) {
        return NULL;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        return espaperplay_reader_epub_blocks(out_cnt);
    }
    if (out_cnt != NULL) {
        *out_cnt = s_txt.block_count;
    }
    return s_txt.blocks;
}

const espaperplay_reader_span_t *espaperplay_reader_spans(int *count) {
    if (count)
        *count = 0;
    return s_open && s_fmt == ESPAPERPLAY_READER_FMT_EPUB ? espaperplay_reader_epub_spans(count)
                                                          : NULL;
}

esp_err_t espaperplay_reader_image(int img_id, int max_w, int max_h,
                                   const lv_image_dsc_t **out_dsc) {
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_fmt != ESPAPERPLAY_READER_FMT_EPUB) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return espaperplay_reader_epub_image(img_id, max_w, max_h, out_dsc);
}

int espaperplay_reader_pagen_load(int chapter, uint32_t font_key, uint32_t *blocks, uint16_t *lines,
                                  int max_cnt) {
    if (!s_open) {
        return 0;
    }
    const uint32_t token =
        s_fmt == ESPAPERPLAY_READER_FMT_EPUB ? espaperplay_reader_epub_token() : s_txt_token;
    return espaperplay_pagen_load(token, chapter, font_key, blocks, lines, max_cnt);
}

int espaperplay_reader_pagen_count(int chapter, uint32_t font_key) {
    const int n = espaperplay_reader_pagen_load(chapter, font_key, NULL, NULL, 0);
    return n < 0 ? -n : 0;
}

void espaperplay_reader_pagen_save_async(int chapter, uint32_t font_key, int cnt,
                                         const uint32_t *blocks, const uint16_t *lines) {
    if (!s_open) {
        return;
    }
    const uint32_t token =
        s_fmt == ESPAPERPLAY_READER_FMT_EPUB ? espaperplay_reader_epub_token() : s_txt_token;
    espaperplay_pagen_save_async(token, chapter, font_key, cnt, blocks, lines);
}

esp_err_t espaperplay_reader_get_text(const char **out_buf, size_t *out_len) {
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        /* EPUB 无整书文本缓冲：返回当前章节文本 */
        const char *t = espaperplay_reader_epub_chapter_text(out_len);
        if (t == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        if (out_buf != NULL) {
            *out_buf = t;
        }
        return ESP_OK;
    }
    if (s_txt.text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_buf != NULL) {
        *out_buf = s_txt.text;
    }
    if (out_len != NULL) {
        *out_len = s_txt.text_len;
    }
    return ESP_OK;
}

esp_err_t espaperplay_reader_open(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const bool is_txt = espaperplay_reader_is_txt(path);
    const bool is_epub = espaperplay_reader_is_epub(path);
    if (!is_txt && !is_epub) {
        ESP_LOGW(TAG, "unsupported file: %s", path);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!espaperplay_storage_is_mounted()) {
        ESP_LOGW(TAG, "storage not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    /* 拼绝对路径：已含挂载点则直接用，否则相对挂载点 */
    char abs_path[256];
    if (strncmp(path, ESPAPERPLAY_STORAGE_MOUNT_POINT, strlen(ESPAPERPLAY_STORAGE_MOUNT_POINT)) ==
        0) {
        strlcpy(abs_path, path, sizeof(abs_path));
    } else {
        /* 去掉开头的 '/' 避免双斜杠 */
        const char *rel = path;
        while (*rel == '/') {
            rel++;
        }
        strlcpy(abs_path, ESPAPERPLAY_STORAGE_MOUNT_POINT, sizeof(abs_path));
        strlcat(abs_path, "/", sizeof(abs_path));
        strlcat(abs_path, rel, sizeof(abs_path));
    }

    struct stat st;
    if (stat(abs_path, &st) != 0) {
        ESP_LOGW(TAG, "stat failed: %s", abs_path);
        return ESP_ERR_NOT_FOUND;
    }
    if (S_ISDIR(st.st_mode)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (is_epub) {
        reader_close_internal(); /* 释放上一份文档（含 EPUB 模块状态） */
        esp_err_t err = espaperplay_reader_epub_open(abs_path);
        if (err != ESP_OK) {
            return err;
        }
        s_fmt = ESPAPERPLAY_READER_FMT_EPUB;
        const char *t = espaperplay_reader_epub_title();
        strlcpy(s_title, (t != NULL && t[0] != '\0') ? t : abs_path, sizeof(s_title));
        const char *base = strrchr(abs_path, '/');
        base = base != NULL ? base + 1 : abs_path;
        if (s_title[0] == '\0') {
            strlcpy(s_title, base, sizeof(s_title));
        }
        strlcpy(s_path, abs_path, sizeof(s_path));
        s_open = true;
        ESP_LOGI(TAG, "opened epub %s", abs_path);
        return ESP_OK;
    }

    return reader_open_txt(abs_path);
}
