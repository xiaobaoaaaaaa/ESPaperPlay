/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - SD 卡二进制缓存通用读写（实现）
 * 见 include/espcache.h 的接口说明。
 */
#include "espcache.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

#include "espaperplay_fs.h"

static const char *TAG = "ESPaperPlay_CACHE";

uint32_t espcache_file_token(const char *abs_path) {
    /* FNV-1a（种子 1073741827、乘子 16777619）路径哈希 ^ mtime ^ size：
     * 书变更（任一变化）自动得到不同 token，旧缓存文件自然失效。 */
    struct stat st;
    uint32_t token = 1073741827u;
    for (const char *c = abs_path; *c != '\0'; c++) {
        token = (token ^ (uint32_t)(unsigned char)*c) * 16777619u;
    }
    if (stat(abs_path, &st) == 0) {
        token ^= (uint32_t)st.st_mtime ^ (uint32_t)st.st_size;
    }
    return token;
}

espcache_result_t espcache_read(const char *path, espcache_read_fn fn, void *ud) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESPCACHE_MISS;
    }
    const espcache_result_t r = fn(f, ud);
    fclose(f);
    if (r == ESPCACHE_CORRUPT) {
        remove(path); /* 损坏 / 失配缓存：删除待重建 */
    }
    return r;
}

bool espcache_write_atomic(const char *path, espcache_write_fn fn, void *ud) {
    /* 父目录惰性逐级创建（EEXIST 快速通过）。 */
    char dir[ESPAPERPLAY_FS_NAME_MAX];
    const char *slash = strrchr(path, '/');
    if (slash == NULL || (size_t)(slash - path) >= sizeof(dir)) {
        return false;
    }
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = '\0';
    if (espaperplay_fs_mkdir_p(dir) != ESP_OK) {
        return false;
    }

    char tmp[ESPAPERPLAY_FS_NAME_MAX + sizeof(ESPCACHE_TMP_SUFFIX)];
    if (snprintf(tmp, sizeof(tmp), "%s" ESPCACHE_TMP_SUFFIX, path) >= (int)sizeof(tmp)) {
        return false;
    }
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return false;
    }
    const bool ok = fn(f, ud);
    fclose(f);
    if (ok) {
        rename(tmp, path); /* 原子替换，避免半写文件被读到 */
        return true;
    }
    remove(tmp);
    return false;
}
