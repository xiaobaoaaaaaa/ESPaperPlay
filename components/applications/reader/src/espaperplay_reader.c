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
#include "espaperplay_reader_history.h"
#include "espaperplay_reader_txt.h"
#include "espaperplay_storage.h"

static const char *TAG = "ESPaperPlay_READER";

/** 单文件最大支持（PSRAM 缓冲，超限拒绝打开）。 */
#define READER_TXT_MAX_BYTES (512 * 1024)

static char *s_buf = NULL;     /*!< 归一化文本缓冲（PSRAM，NUL 结尾） */
static size_t s_len = 0;       /*!< 文本长度（不含 NUL） */
static char s_path[256] = {0}; /*!< 当前文档绝对路径 */
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
    s_len = 0;
    s_path[0] = '\0';
    s_open = false;
}

esp_err_t espaperplay_reader_close(void) {
    reader_close_internal();
    ESP_LOGI(TAG, "reader closed");
    return ESP_OK;
}

bool espaperplay_reader_is_open(void) { return s_open; }

const char *espaperplay_reader_get_path(void) { return s_open ? s_path : NULL; }

esp_err_t espaperplay_reader_get_text(const char **out_buf, size_t *out_len) {
    if (!s_open || s_buf == NULL) {
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

bool espaperplay_reader_is_txt_file(const char *path) { return espaperplay_reader_is_txt(path); }

esp_err_t espaperplay_reader_open(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!espaperplay_reader_is_txt(path)) {
        ESP_LOGW(TAG, "not a txt file: %s", path);
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
    if ((size_t)st.st_size > READER_TXT_MAX_BYTES) {
        ESP_LOGW(TAG, "file too large: %ld bytes", (long)st.st_size);
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(abs_path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "fopen failed: %s", abs_path);
        return ESP_ERR_NOT_FOUND;
    }
    const size_t file_size = (size_t)st.st_size;
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
    strlcpy(s_path, abs_path, sizeof(s_path));
    s_open = true;

    ESP_LOGI(TAG, "opened %s (%u bytes -> %u normalized)", abs_path, (unsigned)file_size,
             (unsigned)s_len);
    return ESP_OK;
}
