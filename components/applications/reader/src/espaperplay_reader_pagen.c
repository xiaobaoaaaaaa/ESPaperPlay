/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_reader_pagen.h"
#include "espcache.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps */
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "espaperplay_config.h"

static const char *TAG = "ESPaperPlay_READER";

/* ====================================================================
 * 分页数据 SD 缓存（TXT / EPUB 通用；token 由调用方按书计算）
 * ==================================================================== */

#define PAGEN_CACHE_DIR ESPAPERPLAY_SYSTEM_SD_DIR "/cache/reader"
#define PAGEN_MAGIC 0x43525046u /* "EPRF"（页边界表） */
#define PAGEN_VER 3u
#define PAGEN_WORKER_STACK 4096 /* 内部 RAM 后备栈（PSRAM 栈优先） */
#define PAGEN_WORKER_PRIO 4

#define PAGEN_QUEUE_LEN 16

/* 有界落盘队列（数组已堆复制）；mutex 保护，worker 消费。 */
typedef struct {
    uint32_t token;
    int chapter;
    uint32_t font_key;
    int cnt;
    uint32_t *blocks;
    uint16_t *lines;
} pagen_job_t;

static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_sem;
static TaskHandle_t s_task;
static uint8_t s_task_state; /* 0=未创建 1=运行 2=不可用 */
static pagen_job_t s_jobs[PAGEN_QUEUE_LEN];
static int s_head, s_tail, s_count;

static void pagen_lock(void) {
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void pagen_unlock(void) {
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

static void pagen_cache_path(char *buf, size_t n, uint32_t token, int chapter, uint32_t font_key) {
    snprintf(buf, n, "%s/%08x.ch%03d.f%08x.pag", PAGEN_CACHE_DIR, (unsigned)token, chapter,
             (unsigned)font_key);
}

/** 读缓存回调：校验头并读入分页表（容量不足走 KEEP，保留文件）。 */
typedef struct {
    uint32_t token;
    int chapter;
    uint32_t font_key;
    uint32_t *blocks;
    uint16_t *lines;
    int max_cnt;
    int cnt; /*!< 出参：读入条数（KEEP 且容量不足时为 -n） */
} pagen_read_ctx_t;

static espcache_result_t pagen_read_fn(FILE *f, void *ud) {
    pagen_read_ctx_t *ctx = ud;
    uint32_t hdr[6] = {0}; /* magic, ver, token, font_key, chapter, cnt */
    if (fread(hdr, sizeof(uint32_t), 6, f) != 6) {
        return ESPCACHE_CORRUPT;
    }
    if (hdr[0] != PAGEN_MAGIC || hdr[1] != PAGEN_VER || hdr[2] != ctx->token ||
        hdr[3] != ctx->font_key || hdr[4] != (uint32_t)ctx->chapter || hdr[5] == 0 ||
        hdr[5] > (uint32_t)((INT_MAX - 24) / 6)) {
        return ESPCACHE_CORRUPT;
    }
    const int n = (int)hdr[5];
    /* Count-only reads must reject truncated tables too. */
    struct stat st;
    if (fstat(fileno(f), &st) != 0) return ESPCACHE_KEEP;
    if (st.st_size != 24 + (off_t)n * 6) return ESPCACHE_CORRUPT;
    if (n > ctx->max_cnt) {
        /* 缓存有效但调用方表容量不足：返回所需容量（负值），由调用方
         * 扩表后重读；绝不删除文件（大章节否则会被误删反复重算） */
        ctx->cnt = -n;
        return ESPCACHE_KEEP;
    }
    if (fread(ctx->blocks, sizeof(uint32_t), (size_t)n, f) != (size_t)n ||
        fread(ctx->lines, sizeof(uint16_t), (size_t)n, f) != (size_t)n) {
        return ESPCACHE_CORRUPT;
    }
    ctx->cnt = n;
    return ESPCACHE_OK;
}

int espaperplay_pagen_load(uint32_t token, int chapter, uint32_t font_key, uint32_t *blocks,
                           uint16_t *lines, int max_cnt) {
    char path[96];
    pagen_cache_path(path, sizeof(path), token, chapter, font_key);
    pagen_read_ctx_t ctx = {
        .token = token,
        .chapter = chapter,
        .font_key = font_key,
        .blocks = blocks,
        .lines = lines,
        .max_cnt = max_cnt,
        .cnt = 0,
    };
    if (espcache_read(path, pagen_read_fn, &ctx) == ESPCACHE_OK) {
        return ctx.cnt;
    }
    return ctx.cnt < 0 ? ctx.cnt : 0;
}

/** 写缓存上下文与回调（头部 + blocks/lines 两段载荷）。 */
typedef struct {
    uint32_t token;
    int chapter;
    uint32_t font_key;
    int cnt;
    const uint32_t *blocks;
    const uint16_t *lines;
} pagen_write_ctx_t;

static bool pagen_write_fn(FILE *f, void *ud) {
    pagen_write_ctx_t *w = ud;
    const uint32_t hdr[6] = {PAGEN_MAGIC, PAGEN_VER,     w->token,
                             w->font_key, (uint32_t)w->chapter, (uint32_t)w->cnt};
    if (fwrite(hdr, sizeof(uint32_t), 6, f) != 6) {
        return false;
    }
    return fwrite(w->blocks, sizeof(uint32_t), (size_t)w->cnt, f) == (size_t)w->cnt &&
           fwrite(w->lines, sizeof(uint16_t), (size_t)w->cnt, f) == (size_t)w->cnt;
}

/** worker：消费有界队列，完整写入后发布缓存。 */
static void pagen_worker_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_sem, portMAX_DELAY);
        pagen_lock();
        if (s_count == 0) { pagen_unlock(); continue; }
        pagen_job_t job = s_jobs[s_head];
        memset(&s_jobs[s_head], 0, sizeof(s_jobs[s_head]));
        s_head = (s_head + 1) % PAGEN_QUEUE_LEN;
        s_count--;
        pagen_unlock();
        if (job.blocks == NULL || job.lines == NULL || job.cnt <= 0) {
            heap_caps_free(job.blocks);
            heap_caps_free(job.lines);
            continue;
        }

        char path[96];
        pagen_cache_path(path, sizeof(path), job.token, job.chapter, job.font_key);
        pagen_write_ctx_t wctx = {
            .token = job.token,
            .chapter = job.chapter,
            .font_key = job.font_key,
            .cnt = job.cnt,
            .blocks = job.blocks,
            .lines = job.lines,
        };
        if (espcache_write_atomic(path, pagen_write_fn, &wctx)) {
            ESP_LOGI(TAG, "pagen: cached ch%d (%d page(s))", job.chapter + 1, job.cnt);
        }
        heap_caps_free(job.blocks);
        heap_caps_free(job.lines);
    }
}

/** 确保落盘 worker 已创建（PSRAM 栈优先，内部 RAM 后备）。 */
static void pagen_worker_ensure(void) {
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_sem == NULL) {
        s_sem = xSemaphoreCreateCounting(PAGEN_QUEUE_LEN, 0);
    }
    if (s_mutex == NULL || s_sem == NULL || s_task_state != 0) {
        return;
    }
    if (xTaskCreateWithCaps(pagen_worker_task, "pagen_w", 4096, NULL, 4, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        s_task_state = 1;
        return;
    }
    if (xTaskCreate(pagen_worker_task, "pagen_w", PAGEN_WORKER_STACK, NULL, PAGEN_WORKER_PRIO,
                    &s_task) == pdPASS) {
        s_task_state = 1;
        return;
    }
    s_task_state = 2;
    s_task = NULL;
    ESP_LOGW(TAG, "pagen: save worker unavailable, caching disabled");
}

void espaperplay_pagen_save_async(uint32_t token, int chapter, uint32_t font_key, int cnt,
                                  const uint32_t *blocks, const uint16_t *lines) {
    if (cnt <= 0 || blocks == NULL || lines == NULL) {
        return;
    }
    pagen_worker_ensure();
    if (s_task_state != 1) {
        return; /* worker 不可用：放弃缓存（下次重算） */
    }
    uint32_t *bcpy =
        heap_caps_malloc((size_t)cnt * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *lcpy =
        heap_caps_malloc((size_t)cnt * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bcpy == NULL || lcpy == NULL) {
        heap_caps_free(bcpy);
        heap_caps_free(lcpy);
        return;
    }
    memcpy(bcpy, blocks, (size_t)cnt * sizeof(uint32_t));
    memcpy(lcpy, lines, (size_t)cnt * sizeof(uint16_t));

    pagen_lock();
    if (s_count == PAGEN_QUEUE_LEN) {
        pagen_unlock();
        heap_caps_free(bcpy);
        heap_caps_free(lcpy);
        ESP_LOGW(TAG, "pagen: save queue full, ch%d will be recomputed", chapter + 1);
        return;
    }
    s_jobs[s_tail] = (pagen_job_t){token, chapter, font_key, cnt, bcpy, lcpy};
    s_tail = (s_tail + 1) % PAGEN_QUEUE_LEN;
    s_count++;
    pagen_unlock();
    xSemaphoreGive(s_sem);
}
