/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "lvgl.h"

#include "espaperplay_config.h"
#include "espaperplay_sd_fonts.h"
#include "espaperplay_storage.h"

static const char *TAG = "ESPaperPlay_SD_FONTS";

/* ====================================================================
 * SD 卡字体目录 → LVGL 文件系统盘符
 * ====================================================================
 *
 * Flash 字体分区由 esp_lv_fs 注册为盘符 'A:'（只读、内存映射、目录固定）。
 * 本驱动额外注册盘符 ESPAPERPLAY_SD_FONT_DRIVE_LETTER（'B:'），把对
 * ESPAPERPLAY_FONTS_SD_DIR（默认 /sdcard/system/fonts）下 '<name>' 的访问
 * 代理到标准 POSIX 文件 API（fopen/fread…），以便 LVGL FreeType 直接以
 * "B:NotoSansSC_Regular.ttf" 打开 SD 卡上的完整字库。该目录仅在 SD 卡挂载
 * 成功后可用（文件系统层自行拦截）。
 *
 * 缓存路径：因为 SD 卡字体可能被后台下载任务替换，这里不做路径级缓存，
 * 每次 open 都实时 fopen（文件不存在即返回 NULL），保证 FreeType 始终
 * 打开最新的文件。
 */

typedef struct {
    FILE *fp;  /*!< 打开的 POSIX 文件句柄 */
    long size; /*!< 打开时的文件大小（用于 SEEK_END 定位） */
} sd_fonts_file_t;

static bool s_drive_registered = false; /*!< 盘符是否已注册（幂等） */

/** 校验文件名合法性：仅允许简单文件名（无路径分隔符 / 目录穿越）。 */
static bool sd_fonts_name_valid(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        return false;
    }
    return true;
}

/** 组装 SD 卡字体文件的绝对 VFS 路径到静态缓冲，返回该路径或 NULL。 */
static const char *sd_fonts_full_path(const char *name) {
    static char path[128];
    if (!sd_fonts_name_valid(name)) {
        return NULL;
    }
    const int written = snprintf(path, sizeof(path), ESPAPERPLAY_FONTS_SD_DIR "/%s", name);
    if (written < 0 || written >= (int)sizeof(path)) {
        return NULL;
    }
    return path;
}

/** 检查 SD 卡上是否存在给定字体文件。 */
bool espaperplay_sd_fonts_exists(const char *file_name) {
    if (!espaperplay_storage_is_mounted()) {
        return false;
    }
    const char *path = sd_fonts_full_path(file_name);
    if (path == NULL) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

static void *sd_fonts_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode) {
    LV_UNUSED(drv);
    if (mode != LV_FS_MODE_RD) {
        ESP_LOGW(TAG, "open '%s': only read mode supported", path);
        return NULL;
    }
    const char *full = sd_fonts_full_path(path);
    if (full == NULL) {
        return NULL;
    }

    sd_fonts_file_t *f = malloc(sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->fp = fopen(full, "rb");
    if (f->fp == NULL) {
        /* 文件不存在或 SD 未挂载——静默返回 NULL，调用方回退 Flash 字体。 */
        free(f);
        return NULL;
    }
    fseek(f->fp, 0, SEEK_END);
    f->size = ftell(f->fp);
    fseek(f->fp, 0, SEEK_SET);
    ESP_LOGD(TAG, "open '%s' (%ld bytes)", full, f->size);
    return f;
}

static lv_fs_res_t sd_fonts_close(lv_fs_drv_t *drv, void *file_p) {
    LV_UNUSED(drv);
    sd_fonts_file_t *f = (sd_fonts_file_t *)file_p;
    if (f == NULL) {
        return LV_FS_RES_FS_ERR;
    }
    fclose(f->fp);
    free(f);
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_fonts_read(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr,
                                 uint32_t *br) {
    LV_UNUSED(drv);
    sd_fonts_file_t *f = (sd_fonts_file_t *)file_p;
    if (f == NULL || f->fp == NULL || buf == NULL || br == NULL) {
        return LV_FS_RES_FS_ERR;
    }
    size_t n = fread(buf, 1, btr, f->fp);
    *br = (uint32_t)n;
    if (n < btr && ferror(f->fp)) {
        return LV_FS_RES_FS_ERR;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_fonts_write(lv_fs_drv_t *drv, void *file_p, const void *buf, uint32_t btw,
                                  uint32_t *bw) {
    LV_UNUSED(drv);
    LV_UNUSED(file_p);
    LV_UNUSED(buf);
    LV_UNUSED(btw);
    LV_UNUSED(bw);
    return LV_FS_RES_DENIED;
}

static lv_fs_res_t sd_fonts_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos,
                                 lv_fs_whence_t whence) {
    LV_UNUSED(drv);
    sd_fonts_file_t *f = (sd_fonts_file_t *)file_p;
    if (f == NULL || f->fp == NULL) {
        return LV_FS_RES_FS_ERR;
    }
    int origin;
    switch (whence) {
    case LV_FS_SEEK_SET:
        origin = SEEK_SET;
        break;
    case LV_FS_SEEK_CUR:
        origin = SEEK_CUR;
        break;
    case LV_FS_SEEK_END:
        origin = SEEK_END;
        break;
    default:
        return LV_FS_RES_INV_PARAM;
    }
    return (fseek(f->fp, (long)pos, origin) == 0) ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t sd_fonts_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p) {
    LV_UNUSED(drv);
    sd_fonts_file_t *f = (sd_fonts_file_t *)file_p;
    if (f == NULL || f->fp == NULL || pos_p == NULL) {
        return LV_FS_RES_FS_ERR;
    }
    long p = ftell(f->fp);
    if (p < 0) {
        return LV_FS_RES_FS_ERR;
    }
    *pos_p = (uint32_t)p;
    return LV_FS_RES_OK;
}

static void *sd_fonts_dir_open(lv_fs_drv_t *drv, const char *path) {
    LV_UNUSED(drv);
    LV_UNUSED(path);
    return NULL;
}

static lv_fs_res_t sd_fonts_dir_read(lv_fs_drv_t *drv, void *dir_p, char *fn, uint32_t fn_len) {
    LV_UNUSED(drv);
    LV_UNUSED(dir_p);
    LV_UNUSED(fn);
    LV_UNUSED(fn_len);
    return LV_FS_RES_DENIED;
}

static lv_fs_res_t sd_fonts_dir_close(lv_fs_drv_t *drv, void *dir_p) {
    LV_UNUSED(drv);
    LV_UNUSED(dir_p);
    return LV_FS_RES_DENIED;
}

esp_err_t espaperplay_sd_fonts_init(void) {
    if (s_drive_registered) {
        return ESP_OK; /* 幂等 */
    }

    /* 驱动注册为静态分配（与 flash 字体不同：LVGL 不提供取消注册，
     * 静态变量避免生命周期问题）。 */
    static lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);
    drv.letter = ESPAPERPLAY_SD_FONT_DRIVE_LETTER;
    drv.cache_size = 0;
    drv.open_cb = sd_fonts_open;
    drv.close_cb = sd_fonts_close;
    drv.read_cb = sd_fonts_read;
    drv.write_cb = sd_fonts_write;
    drv.seek_cb = sd_fonts_seek;
    drv.tell_cb = sd_fonts_tell;
    drv.dir_open_cb = sd_fonts_dir_open;
    drv.dir_read_cb = sd_fonts_dir_read;
    drv.dir_close_cb = sd_fonts_dir_close;
    lv_fs_drv_register(&drv);

    s_drive_registered = true;
    ESP_LOGI(TAG, "SD font drive '%c:' registered -> %s", ESPAPERPLAY_SD_FONT_DRIVE_LETTER,
             ESPAPERPLAY_FONTS_SD_DIR);
    return ESP_OK;
}

esp_err_t espaperplay_sd_fonts_get_path(const char *file_name, char *buf, size_t buf_size) {
    if (file_name == NULL || buf == NULL || buf_size < 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd_fonts_name_valid(file_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written =
        snprintf(buf, buf_size, "%c:%s", ESPAPERPLAY_SD_FONT_DRIVE_LETTER, file_name);
    if (written < 0 || written >= (int)buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
