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
 * @file espaperplay_reader_cover.h
 * @brief 书架封面缩略图服务（后台探测/解码 + SD 缓存，请求-轮询模型）。
 *
 * 供 UI 列表使用：request 入队 → worker 后台完成探测/解码/缓存 → poll 取就绪
 * 结果（LVGL 线程定时轮询，模式与分页缓存/阅读历史 worker 一致）。SD 缓存位
 * 于 /sdcard/system/cache/reader/covers/，键 = 路径哈希 ^ mtime ^ size（书变更
 * 自动失效）；探测失败的负结果同样缓存，避免反复空探。
 */

/** 路径缓冲长度（与阅读历史条目一致）。 */
#define ESPAPERPLAY_READER_COVER_PATH_MAX 256

/** 单条封面就绪结果。w==0 表示无封面 / 不可解码（此时 buf 为 NULL）；否则
 * buf 为 RGB565 像素（PSRAM 堆分配），所有权随 poll 移交调用方，用完
 * heap_caps_free。 */
typedef struct {
    char path[ESPAPERPLAY_READER_COVER_PATH_MAX];
    uint16_t w;
    uint16_t h;
    uint8_t *buf;
} espaperplay_reader_cover_result_t;

/**
 * @brief 请求一本书的封面缩略图（非阻塞，可重复调用去重）。
 *
 * 同一路径在队列 / 就绪结果中已存在时直接返回 ESP_OK。worker 完成后经
 * espaperplay_reader_cover_poll() 取结果。
 *
 * @param path 书籍绝对路径。
 * @param max_w 解码预算框宽（像素）。
 * @param max_h 解码预算框高（像素）。
 * @return ESP_OK 已入队 / 已在途；ESP_ERR_INVALID_STATE SD 未挂载；ESP_ERR_NO_MEM 队列满。
 */
esp_err_t espaperplay_reader_cover_request(const char *path, int max_w, int max_h);

/**
 * @brief 取一条就绪的封面结果（非阻塞）。
 *
 * @param[out] out 结果（含缓冲所有权）。
 * @return 有就绪结果返回 true。
 */
bool espaperplay_reader_cover_poll(espaperplay_reader_cover_result_t *out);

/**
 * @brief 作废全部在途请求与未取结果（翻页 / 切 tab / 退出列表时调用）。
 *
 * 队列清空、未取结果释放；worker 正在处理的任务完成后自行丢弃（代际校验）。
 */
void espaperplay_reader_cover_cancel(void);

#ifdef __cplusplus
}
#endif
