/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_reader_pagen.h"

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

/* 单槽落盘任务（数组已堆复制）；mutex 保护，worker 消费 */
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
static pagen_job_t s_job;
static volatile bool s_job_pending = false;

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

int espaperplay_pagen_load(uint32_t token, int chapter, uint32_t font_key, uint32_t *blocks,
                           uint16_t *lines, int max_cnt) {
    char path[96];
    pagen_cache_path(path, sizeof(path), token, chapter, font_key);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    uint32_t hdr[6] = {0}; /* magic, ver, token, font_key, chapter, cnt */
    int cnt = -1;
    if (fread(hdr, sizeof(uint32_t), 6, f) == 6 && hdr[0] == PAGEN_MAGIC && hdr[1] == PAGEN_VER &&
        hdr[2] == token && hdr[3] == font_key && hdr[4] == (uint32_t)chapter && hdr[5] > 0) {
        const int n = (int)hdr[5];
        if (n > max_cnt) {
            /* 缓存有效但调用方表容量不足：返回所需容量（负值），由调用方
             * 扩表后重读；绝不删除文件（大章节否则会被误删反复重算） */
            fclose(f);
            return -n;
        }
        if (fread(blocks, sizeof(uint32_t), (size_t)n, f) == (size_t)n &&
            fread(lines, sizeof(uint16_t), (size_t)n, f) == (size_t)n) {
            cnt = n;
        }
    }
    fclose(f);
    if (cnt < 0) {
        remove(path); /* 损坏：删除待重建 */
    }
    return cnt;
}

/** worker：把单槽任务写盘（临时文件 + rename 原子替换）。 */
static void pagen_worker_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_sem, portMAX_DELAY);
        pagen_lock();
        pagen_job_t job = s_job;
        s_job_pending = false;
        pagen_unlock();
        if (job.blocks == NULL || job.lines == NULL || job.cnt <= 0) {
            heap_caps_free(job.blocks);
            heap_caps_free(job.lines);
            continue;
        }

        mkdir(ESPAPERPLAY_SYSTEM_SD_DIR, 0755);
        mkdir(ESPAPERPLAY_SYSTEM_SD_DIR "/cache", 0755);
        mkdir(PAGEN_CACHE_DIR, 0755);

        char path[96];
        pagen_cache_path(path, sizeof(path), job.token, job.chapter, job.font_key);
        char tmp[104];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (f != NULL) {
            const uint32_t hdr[6] = {PAGEN_MAGIC,     PAGEN_VER,          job.token,
                                     job.font_key,    (uint32_t)job.chapter, (uint32_t)job.cnt};
            bool ok = fwrite(hdr, sizeof(uint32_t), 6, f) == 6;
            ok = ok && fwrite(job.blocks, sizeof(uint32_t), (size_t)job.cnt, f) == (size_t)job.cnt;
            ok = ok && fwrite(job.lines, sizeof(uint16_t), (size_t)job.cnt, f) == (size_t)job.cnt;
            fclose(f);
            if (ok) {
                rename(tmp, path);
                ESP_LOGI(TAG, "pagen: cached ch%d (%d page(s))", job.chapter + 1, job.cnt);
            } else {
                remove(tmp);
            }
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
        s_sem = xSemaphoreCreateBinary();
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
    if (cnt <= 0) {
        return;
    }
    pagen_worker_ensure();
    if (s_task_state != 1) {
        return; /* worker 不可用：放弃缓存（下次重算） */
    }
    pagen_lock();
    const bool busy = s_job_pending;
    pagen_unlock();
    if (busy) {
        return; /* 上一任务未写完：放弃本次 */
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
    s_job.token = token;
    s_job.chapter = chapter;
    s_job.font_key = font_key;
    s_job.cnt = cnt;
    s_job.blocks = bcpy;
    s_job.lines = lcpy;
    s_job_pending = true;
    pagen_unlock();
    xSemaphoreGive(s_sem);
}
