/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - SD 卡文件系统通用操作（实现）
 * 见 include/espaperplay_fs.h 的接口说明。
 */
#include "espaperplay_fs.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "ESPaperPlay_FS";

esp_err_t espaperplay_fs_mkdir_p(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* 逐级 mkdir：已存在（EEXIST）视为成功。 */
    char buf[ESPAPERPLAY_FS_NAME_MAX];
    const size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
                return ESP_FAIL;
            }
            *p = '/';
        }
    }
    if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool espaperplay_fs_join(char *dst, size_t n, const char *a, const char *b) {
    if (strlcpy(dst, a, n) >= n) {
        return false;
    }
    if (strlcat(dst, "/", n) >= n) {
        return false;
    }
    return strlcat(dst, b, n) < n;
}

const char *espaperplay_fs_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
}

esp_err_t espaperplay_fs_rm_rf(const char *path, int max_depth) {
    if (max_depth < 0) {
        ESP_LOGE(TAG, "rm: depth limit exceeded (%s)", path);
        return ESP_ERR_INVALID_STATE;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_FAIL;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    }
    DIR *d = opendir(path);
    if (d == NULL) {
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char child[ESPAPERPLAY_FS_NAME_MAX];
        if (!espaperplay_fs_join(child, sizeof(child), path, e->d_name)) {
            ESP_LOGW(TAG, "rm: child path too long (%s/%s)", path, e->d_name);
            ret = ESP_ERR_INVALID_SIZE;
            continue;
        }
        const esp_err_t sub = espaperplay_fs_rm_rf(child, max_depth - 1);
        if (sub != ESP_OK) {
            ret = sub;
        }
    }
    closedir(d);
    if (ret == ESP_OK && rmdir(path) != 0) {
        ret = ESP_FAIL;
    }
    return ret;
}

bool espaperplay_fs_name_valid(const char *s) {
    if (s == NULL) {
        return false;
    }
    const size_t n = strlen(s);
    if (n == 0 || n >= ESPAPERPLAY_FS_NAME_MAX) {
        return false;
    }
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) {
        return false;
    }
    /* FAT 禁止尾随空格/点，且部分实现对首尾空格处理不一致，提前拦截。 */
    if (s[n - 1] == ' ' || s[n - 1] == '.') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '/' || c < 0x20 || c == 0x7F) {
            return false;
        }
        /* FAT 保留字符： \ : * ? " < > |  */
        if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|') {
            return false;
        }
    }
    return true;
}
