/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_reader_cover.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps */

#include "espaperplay_config.h"
#include "espaperplay_storage.h"

#include "espaperplay_reader_epub.h"

static const char *TAG = "ESPaperPlay_READER_COVER";

#define COVER_CACHE_DIR ESPAPERPLAY_SYSTEM_SD_DIR "/cache/reader/covers"
#define COVER_CACHE_MAGIC 0x56435045u /* "EPCV" */
#define COVER_CACHE_VER 1u

#define COVER_QUEUE_LEN 16 /* 单页网格容量上限 */
#define COVER_RESULT_LEN 4

#define COVER_WORKER_STACK 8192 /* 内部 RAM；SD 写期间 flash 缓存禁用，栈不能放 PSRAM */
#define COVER_WORKER_PRIO 3     /* 低于 LVGL 与章节预取（解码秒级，尽量不抢 CPU） */

typedef struct {
    char path[ESPAPERPLAY_READER_COVER_PATH_MAX];
    int16_t max_w;
    int16_t max_h;
} cover_job_t;

static SemaphoreHandle_t s_mutex;      /* 队列 / 结果 / 代际互斥 */
static SemaphoreHandle_t s_job_sem;    /* 计数 = 待处理任务数 */
static TaskHandle_t s_worker = NULL;

static cover_job_t s_jobs[COVER_QUEUE_LEN];
static int s_job_head = 0, s_job_tail = 0, s_job_cnt = 0;

static espaperplay_reader_cover_result_t s_results[COVER_RESULT_LEN];
static int s_result_cnt = 0;

static uint32_t s_gen = 0; /* cancel 代际：作废在途任务 */

/* ------------------------------------------------------------------ */
/* SD 缓存                                                              */
/* ------------------------------------------------------------------ */

/** 封面缓存路径（书指纹 = 路径哈希 ^ mtime ^ size，与章节缓存同方案）。 */
static void cover_cache_path(char *buf, size_t n, const char *path, uint32_t src_size,
                             uint32_t src_mtime) {
    uint32_t token = 1073741827u;
    const char *p = path;
    while (*p != '\0') {
        token = (token ^ (uint32_t)(unsigned char)*p++) * 16777619u; /* FNV-1a */
    }
    token ^= src_mtime ^ src_size;
    snprintf(buf, n, "%s/%08x.cov", COVER_CACHE_DIR, (unsigned)token);
}

/** 读缓存。命中返回 true（含负缓存：w==0 无封面）。 */
static bool cover_cache_load(const char *cache_path, uint32_t src_size, uint32_t src_mtime,
                             espaperplay_reader_cover_result_t *out) {
    FILE *f = fopen(cache_path, "rb");
    if (f == NULL) {
        return false;
    }
    uint32_t hdr[6] = {0}; /* magic, ver, w, h, src_size, src_mtime */
    bool ok = false;
    do {
        if (fread(hdr, sizeof(uint32_t), 6, f) != 6) {
            break;
        }
        if (hdr[0] != COVER_CACHE_MAGIC || hdr[1] != COVER_CACHE_VER ||
            hdr[4] != src_size || hdr[5] != src_mtime) {
            break;
        }
        const uint32_t w = hdr[2];
        const uint32_t h = hdr[3];
        if (w == 0 || h == 0) {
            if (w == 0 && h == 0) { /* 负缓存：无封面 */
                out->w = 0;
                out->h = 0;
                out->buf = NULL;
                ok = true;
            }
            break;
        }
        if (w > 1024 || h > 1024) {
            break;
        }
        uint8_t *buf = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            break;
        }
        if (fread(buf, 1, (size_t)w * h * 2, f) != (size_t)w * h * 2) {
            heap_caps_free(buf);
            break;
        }
        out->w = (uint16_t)w;
        out->h = (uint16_t)h;
        out->buf = buf;
        ok = true;
    } while (false);
    fclose(f);
    if (!ok) {
        remove(cache_path); /* 损坏 / 失配缓存：删除待重建 */
    }
    return ok;
}

/** 写缓存（仅 worker 调用）。data==NULL 时写负缓存（w=h=0）。 */
static void cover_cache_write(const char *cache_path, uint32_t src_size, uint32_t src_mtime, int w,
                              int h, const uint8_t *data) {
    mkdir(ESPAPERPLAY_SYSTEM_SD_DIR, 0755);
    mkdir(ESPAPERPLAY_SYSTEM_SD_DIR "/cache", 0755);
    mkdir(ESPAPERPLAY_SYSTEM_SD_DIR "/cache/reader", 0755);
    mkdir(COVER_CACHE_DIR, 0755);
    char tmp[sizeof(COVER_CACHE_DIR) + 24];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cache_path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return;
    }
    const uint32_t hdr[6] = {COVER_CACHE_MAGIC, COVER_CACHE_VER, (uint32_t)w,
                             (uint32_t)h,       src_size,        src_mtime};
    bool ok = fwrite(hdr, sizeof(uint32_t), 6, f) == 6;
    if (ok && data != NULL) {
        ok = fwrite(data, 1, (size_t)w * h * 2, f) == (size_t)w * h * 2;
    }
    fclose(f);
    if (ok) {
        rename(tmp, cache_path); /* 原子替换，避免半写文件被读到 */
    } else {
        remove(tmp);
    }
}

/* ------------------------------------------------------------------ */
/* 处理流程（worker）                                                    */
/* ------------------------------------------------------------------ */

/** 取互斥锁下当前代际。 */
static uint32_t cover_gen_now(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const uint32_t gen = s_gen;
    xSemaphoreGive(s_mutex);
    return gen;
}

/** 投递结果（代际已作废则丢弃并释放缓冲；结果环满覆盖最旧）。 */
static void cover_result_push(uint32_t gen, const espaperplay_reader_cover_result_t *res) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (gen != s_gen) {
        xSemaphoreGive(s_mutex);
        if (res->buf != NULL) {
            heap_caps_free(res->buf);
        }
        return;
    }
    if (s_result_cnt >= COVER_RESULT_LEN) { /* 覆盖最旧（UI 轮询不过来时兜底） */
        heap_caps_free(s_results[0].buf);
        memmove(&s_results[0], &s_results[1],
                sizeof(espaperplay_reader_cover_result_t) * (COVER_RESULT_LEN - 1));
        s_result_cnt = COVER_RESULT_LEN - 1;
    }
    s_results[s_result_cnt++] = *res;
    xSemaphoreGive(s_mutex);
}

static void cover_process(const cover_job_t *job) {
    const uint32_t gen = cover_gen_now();

    /* 源文件指纹（书变更自动换缓存名失效） */
    struct stat st;
    uint32_t src_size = 0;
    uint32_t src_mtime = 0;
    if (stat(job->path, &st) == 0) {
        src_size = (uint32_t)st.st_size;
        src_mtime = (uint32_t)st.st_mtime;
    }
    char cache_path[64];
    cover_cache_path(cache_path, sizeof(cache_path), job->path, src_size, src_mtime);

    espaperplay_reader_cover_result_t res;
    memset(&res, 0, sizeof(res));
    strlcpy(res.path, job->path, sizeof(res.path));

    if (cover_cache_load(cache_path, src_size, src_mtime, &res)) {
        cover_result_push(gen, &res);
        return;
    }

    /* 探测 + 解码（预算框内） */
    lv_image_dsc_t dsc;
    uint8_t *buf = NULL;
    const esp_err_t err =
        espaperplay_reader_epub_probe_cover(job->path, job->max_w, job->max_h, &dsc, &buf);
    if (err == ESP_OK) {
        res.w = (uint16_t)dsc.header.w;
        res.h = (uint16_t)dsc.header.h;
        res.buf = buf;
        cover_cache_write(cache_path, src_size, src_mtime, res.w, res.h, buf);
    } else if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED || err == ESP_FAIL) {
        /* 无封面 / 格式不支持 / 解码失败：负缓存，避免每次翻页空探 */
        cover_cache_write(cache_path, src_size, src_mtime, 0, 0, NULL);
        res.w = 0;
        res.h = 0;
        res.buf = NULL;
    } else {
        /* 瞬时错误（内存 / IO）：不写缓存，下次重试 */
        ESP_LOGW(TAG, "cover: probe %s failed (%s)", job->path, esp_err_to_name(err));
        res.w = 0;
        res.h = 0;
        res.buf = NULL;
    }
    cover_result_push(gen, &res);
}

static void cover_worker_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_job_sem, portMAX_DELAY);
        cover_job_t job;
        bool have = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_job_cnt > 0) {
            job = s_jobs[s_job_head];
            s_job_head = (s_job_head + 1) % COVER_QUEUE_LEN;
            s_job_cnt--;
            have = true;
        }
        xSemaphoreGive(s_mutex);
        if (have) {
            cover_process(&job);
        }
    }
}

/** 确保 worker 已创建（内部 RAM 栈，创建失败则本轮请求不服务）。 */
static void cover_worker_ensure(void) {
    if (s_worker != NULL) {
        return;
    }
    if (xTaskCreateWithCaps(cover_worker_task, "rdr_cover", COVER_WORKER_STACK, NULL,
                            COVER_WORKER_PRIO, &s_worker,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "cover worker create failed");
    }
}

/* ------------------------------------------------------------------ */
/* 对外 API                                                              */
/* ------------------------------------------------------------------ */

/** 队列 / 结果环中是否已有同路径任务或结果。 */
static bool cover_pending_has(const char *path) {
    for (int i = 0; i < s_job_cnt; i++) {
        const int idx = (s_job_head + i) % COVER_QUEUE_LEN;
        if (strcmp(s_jobs[idx].path, path) == 0) {
            return true;
        }
    }
    for (int i = 0; i < s_result_cnt; i++) {
        if (strcmp(s_results[i].path, path) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t espaperplay_reader_cover_request(const char *path, int max_w, int max_h) {
    if (path == NULL || max_w <= 0 || max_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!espaperplay_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        s_job_sem = xSemaphoreCreateCounting(COVER_QUEUE_LEN, 0);
        if (s_mutex == NULL || s_job_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!cover_pending_has(path)) {
        if (s_job_cnt >= COVER_QUEUE_LEN) {
            ret = ESP_ERR_NO_MEM;
        } else {
            cover_job_t *job = &s_jobs[s_job_tail];
            memset(job, 0, sizeof(*job));
            strlcpy(job->path, path, sizeof(job->path));
            job->max_w = (int16_t)(max_w > 512 ? 512 : max_w);
            job->max_h = (int16_t)(max_h > 512 ? 512 : max_h);
            s_job_tail = (s_job_tail + 1) % COVER_QUEUE_LEN;
            s_job_cnt++;
            xSemaphoreGive(s_job_sem);
            cover_worker_ensure();
        }
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

bool espaperplay_reader_cover_poll(espaperplay_reader_cover_result_t *out) {
    if (out == NULL || s_mutex == NULL) {
        return false;
    }
    bool have = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_result_cnt > 0) {
        *out = s_results[0];
        memmove(&s_results[0], &s_results[1],
                sizeof(espaperplay_reader_cover_result_t) * (COVER_RESULT_LEN - 1));
        s_result_cnt--;
        have = true;
    }
    xSemaphoreGive(s_mutex);
    return have;
}

void espaperplay_reader_cover_cancel(void) {
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_gen++; /* 在途任务完成后自行丢弃 */
    s_job_head = 0;
    s_job_tail = 0;
    s_job_cnt = 0;
    for (int i = 0; i < s_result_cnt; i++) {
        if (s_results[i].buf != NULL) {
            heap_caps_free(s_results[i].buf);
        }
    }
    s_result_cnt = 0;
    xSemaphoreGive(s_mutex);
}
