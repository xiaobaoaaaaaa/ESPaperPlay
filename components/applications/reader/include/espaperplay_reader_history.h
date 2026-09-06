/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_reader_history.h
 * @brief 阅读历史管理（持久化到 SD 卡文本文件）。
 *
 * 历史记录保存到 SD 卡系统目录（ESPAPERPLAY_SYSTEM_SD_DIR "/reader/
 * history.txt"，UTF-8 行格式：路径<TAB>页码<TAB>总页数<TAB>unix 时间）。
 * 路径不含 TAB/换行（FAT 文件名禁止控制字符），可安全作分隔符。
 *
 * 读操作（load / find）为同步只读，可在 LVGL 线程直接调用；写操作
 * （update / remove / clear）内部投递到专用 worker 任务（内部 RAM 栈）执行
 * ——SD 写期间 flash 缓存被禁用，不能占用 PSRAM 栈的 LVGL 线程。调用方可
 * 用 espaperplay_reader_history_flush() 等待写操作完成。
 */

/** 历史记录最大条数（超出时淘汰最旧）。 */
#define ESPAPERPLAY_READER_HISTORY_MAX 20

/** 历史条目。 */
typedef struct {
    char path[256];  /*!< 文档绝对路径 */
    uint32_t page;   /*!< 当前页（0 基） */
    uint32_t total;  /*!< 保存时的总页数（0=未知） */
    int64_t unix_ts; /*!< 上次阅读时间（unix 秒；0=未知） */
} espaperplay_reader_history_entry_t;

/**
 * @brief 初始化阅读历史模块（惰性：worker 首次写时创建）。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t espaperplay_reader_history_init(void);

/**
 * @brief 读取全部历史（同步，只读；SD 未挂载返回 count=0）。
 *
 * @param[out] entries 接收条目的数组（调用方提供，至少 max 项）。
 * @param[in]  max     数组容量。
 * @param[out] count   实际条数（按最近阅读排序）。
 *
 * @return ESP_OK（可能 count=0）；参数非法返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t espaperplay_reader_history_load(espaperplay_reader_history_entry_t *entries, int max,
                                          int *count);

/**
 * @brief 查找指定路径的历史（同步，只读）。
 *
 * @param path 文档绝对路径。
 * @param[out] out 命中时返回条目（最近一次）。
 *
 * @return 命中返回 ESP_OK；无历史或参数非法返回相应错误码。
 */
esp_err_t espaperplay_reader_history_find(const char *path,
                                          espaperplay_reader_history_entry_t *out);

/**
 * @brief 记录/更新阅读进度（异步投递，不阻塞调用方）。
 *
 * 已存在同路径条目时移到最前并更新；否则插入最前；超出上限淘汰最旧。
 *
 * @param path  文档绝对路径。
 * @param page  当前页（0 基）。
 * @param total 总页数（0=未知）。
 *
 * @return 已投递返回 ESP_OK；SD 未挂载返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_reader_history_update(const char *path, uint32_t page, uint32_t total);

/**
 * @brief 删除指定路径的历史（异步投递）。
 *
 * @return 已投递返回 ESP_OK；SD 未挂载返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_reader_history_remove(const char *path);

/**
 * @brief 清空全部历史（异步投递）。
 *
 * @return 已投递返回 ESP_OK；SD 未挂载返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_reader_history_clear(void);

/**
 * @brief 等待所有已投递的历史写操作完成。
 *
 * @param timeout_ms 超时（毫秒）。
 *
 * @return 排空返回 ESP_OK；超时返回 ESP_ERR_TIMEOUT。
 */
esp_err_t espaperplay_reader_history_flush(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
