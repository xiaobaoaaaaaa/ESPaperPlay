/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - SD 卡文件系统通用操作
 *
 * 此前逐级 mkdir 链、路径拼接、递归删除、条目名校验在各模块（reader 缓存、
 * 文件管理页、Web 文件接口、字体上传）各写一份且行为已经发散（Web 端校验
 * 比设备端宽松，可创建设备端拒绝的文件名）——统一收敛到本模块。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** FAT LFN 单条目名上限（255 UTF-8 字节）+ NUL。 */
#define ESPAPERPLAY_FS_NAME_MAX 256

/**
 * @brief 逐级创建目录（FAT 的 mkdir 不支持一次建多级）。
 * @return ESP_OK=已存在或创建成功；否则失败码。
 */
esp_err_t espaperplay_fs_mkdir_p(const char *path);

/**
 * @brief 拼接 a + "/" + b（strlcpy/strlcat，恒 NUL 结尾）。
 * @return false=缓冲不足（不写半截路径）。
 */
bool espaperplay_fs_join(char *dst, size_t n, const char *a, const char *b);

/** @brief 取路径最后一段（无 '/' 或以 '/' 结尾时返回原串）。 */
const char *espaperplay_fs_basename(const char *path);

/**
 * @brief 递归删除文件 / 目录（深度受限，防御异常嵌套与符号环）。
 * @param max_depth 最大目录深度（超过返回 ESP_ERR_INVALID_STATE）。
 */
esp_err_t espaperplay_fs_rm_rf(const char *path, int max_depth);

/**
 * @brief SD 卡条目名合法性（FAT 严格版，设备端与 Web 端共用同一规则）：
 * 非空、非 "."/".."、不含 '/' 与控制字符、无 FAT 保留字符 \ : * ? " < > |、
 * 不以空格或点结尾。
 */
bool espaperplay_fs_name_valid(const char *name);

#ifdef __cplusplus
}
#endif
