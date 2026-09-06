/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file espaperplay_lv_mem_psram.c
 *
 * LVGL 内存后端（CONFIG_LV_USE_CUSTOM_MALLOC）：所有经 lv_malloc 的分配
 * —— FreeType 库本体与字形三层缓存、widget/style 等 —— PSRAM 优先，
 * PSRAM 不足回落内部 RAM。LVGL 自身结构体不参与 DMA；本板唯一过 SPI
 * DMA 的是 gui 后端自管的 EPD draw buffer，本就分配在 PSRAM。
 */

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#include "lvgl.h"

void lv_mem_init(void) {
    /* 系统堆由 ESP-IDF 初始化，此处无需处理。 */
}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    (void)mem;
    (void)bytes;
    return NULL; /* 不支持静态池 */
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    (void)pool;
}

void *lv_malloc_core(size_t size) {
    if (size == 0) {
        size = 1; /* 保证可 free，且调用方（LVGL）按非 NULL 判定成功 */
    }
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

void *lv_realloc_core(void *p, size_t new_size) {
    if (new_size == 0) {
        new_size = 1;
    }
    if (p == NULL) {
        return lv_malloc_core(new_size);
    }
    /* 按原块位置钉住 caps：防止 realloc 搬家时 PSRAM 块漂移回内部 RAM。 */
    uint32_t caps =
        esp_ptr_external_ram(p) ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_8BIT;
    return heap_caps_realloc(p, new_size, caps);
}

void lv_free_core(void *p) {
    free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
    memset(mon_p, 0, sizeof(*mon_p));
    mon_p->total_size = (size_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon_p->free_size = (size_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon_p->free_biggest_size = (size_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (mon_p->total_size > 0) {
        mon_p->used_pct =
            (uint8_t)((mon_p->total_size - mon_p->free_size) * 100 / mon_p->total_size);
    }
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}
