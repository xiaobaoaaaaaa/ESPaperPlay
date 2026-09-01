/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_reader.h"

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
#include "espaperplay_reader_txt.h"
#include "espaperplay_storage.h"

static const char *TAG = "ESPaperPlay_READER";

/** 单个 TXT 文件最大支持（PSRAM 缓冲，超限拒绝打开）。 */
#define READER_TXT_MAX_BYTES (512 * 1024)

/* TXT 文档状态（单章节：文本 blob + 行块表） */
static char *s_buf = NULL;      /*!< 归一化文本缓冲（PSRAM，NUL 结尾） */
static size_t s_len = 0;        /*!< 文本长度（不含 NUL） */
static espaperplay_reader_block_t *s_txt_blocks = NULL; /*!< TXT 行块表 */
static int s_txt_block_cnt = 0;

static espaperplay_reader_fmt_t s_fmt = ESPAPERPLAY_READER_FMT_NONE;
static char s_path[256] = {0}; /*!< 当前文档绝对路径 */
static char s_title[128] = {0};
static bool s_open = false;    /*!< 是否已打开 */

esp_err_t espaperplay_reader_init(void) {
    ESP_LOGI(TAG, "Reader framework init");
    (void)espaperplay_reader_history_init();
    return ESP_OK;
}

static void reader_close_internal(void) {
    if (s_buf != NULL) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    if (s_txt_blocks != NULL) {
        heap_caps_free(s_txt_blocks);
        s_txt_blocks = NULL;
    }
    s_txt_block_cnt = 0;
    s_len = 0;
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

/** TXT 行块表：每非空行为一块（块内无 '\n'；空行为占位块分隔段落间距）。 */
static esp_err_t reader_txt_build_blocks(void) {
    /* 预估行数（按 '\n' 计数 + 1） */
    size_t lines = 1;
    for (size_t i = 0; i < s_len; i++) {
        if (s_buf[i] == '\n') {
            lines++;
        }
    }
    if (lines > 65535 * 4) { /* 块表上限（防御病态文件） */
        lines = 65535 * 4;
    }
    s_txt_blocks = heap_caps_malloc(lines * sizeof(espaperplay_reader_block_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_txt_blocks == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t off = 0;
    int n = 0;
    while (off <= s_len && n < (int)lines) {
        char *nl = memchr(&s_buf[off], '\n', s_len - off);
        const size_t len = nl != NULL ? (size_t)(nl - &s_buf[off]) : s_len - off;
        s_txt_blocks[n].off = (uint32_t)off;
        s_txt_blocks[n].len = (uint32_t)len;
        s_txt_blocks[n].flags = 0;
        s_txt_blocks[n].image = -1;
        n++;
        if (nl == NULL) {
            break;
        }
        off += len + 1;
    }
    s_txt_block_cnt = n;
    return ESP_OK;
}

/** TXT 打开（读取 + 归一化 + 行块表）。 */
static esp_err_t reader_open_txt(const char *abs_path, size_t file_size) {
    FILE *f = fopen(abs_path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "fopen failed: %s", abs_path);
        return ESP_ERR_NOT_FOUND;
    }
    char *buf = heap_caps_malloc(file_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read_n = 0;
    if (file_size > 0) {
        read_n = fread(buf, 1, file_size, f);
        if (read_n != file_size) {
            ESP_LOGW(TAG, "fread short: %u/%u", (unsigned)read_n, (unsigned)file_size);
            /* 按实际读取长度继续 */
        }
    }
    fclose(f);
    buf[read_n] = '\0';

    /* 归一化（BOM + 换行） */
    size_t norm_len = read_n;
    espaperplay_reader_txt_normalize(buf, &norm_len);

    reader_close_internal();
    s_buf = buf;
    s_len = norm_len;
    s_fmt = ESPAPERPLAY_READER_FMT_TXT;

    const esp_err_t err = reader_txt_build_blocks();
    if (err != ESP_OK) {
        reader_close_internal();
        return err;
    }

    /* 标题：文件名去后缀 */
    const char *base = strrchr(abs_path, '/');
    base = base != NULL ? base + 1 : abs_path;
    strlcpy(s_title, base, sizeof(s_title));
    char *dot = strrchr(s_title, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    strlcpy(s_path, abs_path, sizeof(s_path));
    s_open = true;

    ESP_LOGI(TAG, "opened txt %s (%u bytes -> %u normalized, %d lines)", abs_path,
             (unsigned)file_size, (unsigned)s_len, s_txt_block_cnt);
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
    return s_fmt == ESPAPERPLAY_READER_FMT_EPUB ? espaperplay_reader_epub_chapter_count() : 1;
}

esp_err_t espaperplay_reader_load_chapter(int idx) {
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_TXT) {
        return idx == 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    return espaperplay_reader_epub_load_chapter(idx);
}

int espaperplay_reader_chapter_current(void) {
    if (!s_open) {
        return -1;
    }
    return s_fmt == ESPAPERPLAY_READER_FMT_EPUB ? espaperplay_reader_epub_chapter_current() : 0;
}

bool espaperplay_reader_poll_chapter(int idx) {
    if (!s_open) {
        return false;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        return espaperplay_reader_epub_poll_chapter(idx);
    }
    return idx == 0;
}

const char *espaperplay_reader_chapter_title(void) {
    if (!s_open) {
        return NULL;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        return espaperplay_reader_epub_chapter_title();
    }
    return s_title;
}

const char *espaperplay_reader_chapter_text(size_t *out_len) {
    if (!s_open) {
        return NULL;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        return espaperplay_reader_epub_chapter_text(out_len);
    }
    if (out_len != NULL) {
        *out_len = s_len;
    }
    return s_buf;
}

const espaperplay_reader_block_t *espaperplay_reader_blocks(int *out_cnt) {
    if (!s_open) {
        return NULL;
    }
    if (s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        return espaperplay_reader_epub_blocks(out_cnt);
    }
    if (out_cnt != NULL) {
        *out_cnt = s_txt_block_cnt;
    }
    return s_txt_blocks;
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

bool espaperplay_reader_image_poll(int expect_id) {
    if (!s_open || s_fmt != ESPAPERPLAY_READER_FMT_EPUB) {
        return false;
    }
    return espaperplay_reader_epub_image_poll(expect_id);
}

void espaperplay_reader_image_cancel(void) {
    if (s_open && s_fmt == ESPAPERPLAY_READER_FMT_EPUB) {
        espaperplay_reader_epub_image_cancel();
    }
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
    if (s_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_buf != NULL) {
        *out_buf = s_buf;
    }
    if (out_len != NULL) {
        *out_len = s_len;
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
    if (is_txt && (size_t)st.st_size > READER_TXT_MAX_BYTES) {
        ESP_LOGW(TAG, "file too large: %ld bytes", (long)st.st_size);
        return ESP_ERR_NO_MEM;
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

    return reader_open_txt(abs_path, (size_t)st.st_size);
}
