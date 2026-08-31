/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_reader_history.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps */
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "espaperplay_config.h"
#include "espaperplay_storage.h"

static const char *TAG = "ESPaperPlay_READER";

/** 历史文件路径（SD 卡系统目录）。 */
#define HIST_FILE_DIR ESPAPERPLAY_SYSTEM_SD_DIR "/reader"
#define HIST_FILE_PATH HIST_FILE_DIR "/history.txt"

#define HIST_LINE_MAX 300      /* 单行缓冲（路径 256 + 分隔符 + 数值） */
#define HIST_QUEUE_LEN 8       /* 写操作队列长度 */
#define HIST_WORKER_STACK 8192 /* worker 栈（内部 RAM；含 20 条目数组 ~5.4KB） */
#define HIST_WORKER_PRIO 4

/** 写操作类型。 */
typedef enum {
    HIST_JOB_UPDATE = 0,
    HIST_JOB_REMOVE,
    HIST_JOB_CLEAR,
} hist_job_type_t;

/** worker 操作项（路径为副本，跨任务安全）。 */
typedef struct {
    hist_job_type_t type;
    char path[256];
    uint32_t page;
    uint32_t total;
} hist_work_t;

static QueueHandle_t s_hist_queue = NULL; /*!< 写操作队列 */
static TaskHandle_t s_hist_worker = NULL; /*!< worker 任务（内部 RAM 栈） */
static volatile int s_hist_pending = 0;   /*!< 未完成写操作计数（flush 轮询） */

/* ------------------------------------------------------------------ */
/* 文件读写（worker 内调用；load 为只读可在任意线程调用）                  */
/* ------------------------------------------------------------------ */

/** 解析一行 "path\tpage\ttotal\tunix"；成功返回 0。 */
static int hist_parse_line(char *line, espaperplay_reader_history_entry_t *e) {
    char *tab1 = strchr(line, '\t');
    if (tab1 == NULL) {
        return -1;
    }
    *tab1 = '\0';
    char *tab2 = strchr(tab1 + 1, '\t');
    if (tab2 == NULL) {
        return -1;
    }
    *tab2 = '\0';
    char *tab3 = strchr(tab2 + 1, '\t');
    if (tab3 == NULL) {
        return -1;
    }
    *tab3 = '\0';
    char *nl = strchr(tab3 + 1, '\n');
    if (nl != NULL) {
        *nl = '\0';
    }
    if (line[0] == '\0') {
        return -1;
    }
    strlcpy(e->path, line, sizeof(e->path));
    e->page = (uint32_t)strtoul(tab1 + 1, NULL, 10);
    e->total = (uint32_t)strtoul(tab2 + 1, NULL, 10);
    e->unix_ts = (int64_t)strtoll(tab3 + 1, NULL, 10);
    return 0;
}

/** 确保历史目录存在（mkdir 已存在时静默忽略）。 */
static void hist_ensure_dir(void) {
    (void)mkdir(ESPAPERPLAY_SYSTEM_SD_DIR, 0775);
    (void)mkdir(HIST_FILE_DIR, 0775);
}

/** 把全部条目写回文件（最前 = 最近）。 */
static esp_err_t hist_write_all(const espaperplay_reader_history_entry_t *entries, int count) {
    hist_ensure_dir();
    FILE *f = fopen(HIST_FILE_PATH, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "history: fopen write failed");
        return ESP_FAIL;
    }
    for (int i = 0; i < count; i++) {
        fprintf(f, "%s\t%u\t%u\t%lld\n", entries[i].path, (unsigned)entries[i].page,
                (unsigned)entries[i].total, (long long)entries[i].unix_ts);
    }
    fclose(f);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 写操作处理（worker 内）                                              */
/* ------------------------------------------------------------------ */

/** UPDATE：存在则移到最前并更新，否则插入最前；超出上限淘汰最旧。 */
static void hist_apply_update(const char *path, uint32_t page, uint32_t total) {
    espaperplay_reader_history_entry_t arr[ESPAPERPLAY_READER_HISTORY_MAX];
    int cnt = 0;
    (void)espaperplay_reader_history_load(arr, ESPAPERPLAY_READER_HISTORY_MAX, &cnt);

    int idx = -1;
    for (int i = 0; i < cnt; i++) {
        if (strcmp(arr[i].path, path) == 0) {
            idx = i;
            break;
        }
    }
    if (idx >= 0) {
        for (int i = idx; i > 0; i--) {
            arr[i] = arr[i - 1];
        }
        arr[0].page = page;
        arr[0].total = total;
        arr[0].unix_ts = (int64_t)time(NULL);
        strlcpy(arr[0].path, path, sizeof(arr[0].path));
    } else {
        if (cnt >= ESPAPERPLAY_READER_HISTORY_MAX) {
            cnt = ESPAPERPLAY_READER_HISTORY_MAX - 1;
        }
        for (int i = cnt; i > 0; i--) {
            arr[i] = arr[i - 1];
        }
        strlcpy(arr[0].path, path, sizeof(arr[0].path));
        arr[0].page = page;
        arr[0].total = total;
        arr[0].unix_ts = (int64_t)time(NULL);
        cnt++;
    }
    if (hist_write_all(arr, cnt) == ESP_OK) {
        ESP_LOGI(TAG, "history: update %s (page %u/%u)", path, (unsigned)page, (unsigned)total);
    }
}

/** REMOVE：删除匹配条目并写回。 */
static void hist_apply_remove(const char *path) {
    espaperplay_reader_history_entry_t arr[ESPAPERPLAY_READER_HISTORY_MAX];
    int cnt = 0;
    (void)espaperplay_reader_history_load(arr, ESPAPERPLAY_READER_HISTORY_MAX, &cnt);

    int idx = -1;
    for (int i = 0; i < cnt; i++) {
        if (strcmp(arr[i].path, path) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        ESP_LOGI(TAG, "history: remove %s (not found)", path);
        return;
    }
    for (int i = idx; i < cnt - 1; i++) {
        arr[i] = arr[i + 1];
    }
    cnt--;
    (void)hist_write_all(arr, cnt);
    ESP_LOGI(TAG, "history: remove %s", path);
}

/** CLEAR：删除历史文件。 */
static void hist_apply_clear(void) {
    if (unlink(HIST_FILE_PATH) == 0 || errno == ENOENT) {
        ESP_LOGI(TAG, "history: cleared");
    } else {
        ESP_LOGW(TAG, "history: clear failed (errno=%d)", errno);
    }
}

/** worker 任务（内部 RAM 栈；SD 写期间 flash 缓存禁用，栈不能放 PSRAM）。 */
static void hist_worker_task(void *arg) {
    (void)arg;
    hist_work_t w;
    for (;;) {
        if (xQueueReceive(s_hist_queue, &w, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (w.type == HIST_JOB_UPDATE) {
            hist_apply_update(w.path, w.page, w.total);
        } else if (w.type == HIST_JOB_REMOVE) {
            hist_apply_remove(w.path);
        } else {
            hist_apply_clear();
        }
        s_hist_pending--;
    }
}

/** 确保 worker 与队列已创建并投递一个写操作（非阻塞）。 */
static esp_err_t hist_post(const hist_work_t *w) {
    if (!espaperplay_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_hist_queue == NULL) {
        s_hist_queue = xQueueCreate(HIST_QUEUE_LEN, sizeof(hist_work_t));
    }
    if (s_hist_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (s_hist_worker == NULL) {
        if (xTaskCreateWithCaps(hist_worker_task, "rdr_hist", HIST_WORKER_STACK, NULL,
                                HIST_WORKER_PRIO, &s_hist_worker,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "history worker create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (xQueueSend(s_hist_queue, w, 0) != pdTRUE) {
        ESP_LOGW(TAG, "history: queue full, op dropped");
        return ESP_ERR_NO_MEM;
    }
    s_hist_pending++;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                             */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_reader_history_init(void) {
    ESP_LOGI(TAG, "reader history module ready");
    return ESP_OK;
}

esp_err_t espaperplay_reader_history_load(espaperplay_reader_history_entry_t *entries, int max,
                                          int *count) {
    if (entries == NULL || count == NULL || max <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (!espaperplay_storage_is_mounted()) {
        return ESP_OK;
    }
    FILE *f = fopen(HIST_FILE_PATH, "r");
    if (f == NULL) {
        return ESP_OK; /* 尚无历史文件 */
    }
    char line[HIST_LINE_MAX];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f) != NULL) {
        if (hist_parse_line(line, &entries[n]) == 0) {
            n++;
        }
    }
    fclose(f);
    *count = n;
    return ESP_OK;
}

esp_err_t espaperplay_reader_history_find(const char *path,
                                          espaperplay_reader_history_entry_t *out) {
    if (path == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    espaperplay_reader_history_entry_t arr[ESPAPERPLAY_READER_HISTORY_MAX];
    int cnt = 0;
    (void)espaperplay_reader_history_load(arr, ESPAPERPLAY_READER_HISTORY_MAX, &cnt);
    for (int i = 0; i < cnt; i++) {
        if (strcmp(arr[i].path, path) == 0) {
            *out = arr[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t espaperplay_reader_history_update(const char *path, uint32_t page, uint32_t total) {
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    hist_work_t w = {0};
    w.type = HIST_JOB_UPDATE;
    w.page = page;
    w.total = total;
    strlcpy(w.path, path, sizeof(w.path));
    return hist_post(&w);
}

esp_err_t espaperplay_reader_history_remove(const char *path) {
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    hist_work_t w = {0};
    w.type = HIST_JOB_REMOVE;
    strlcpy(w.path, path, sizeof(w.path));
    return hist_post(&w);
}

esp_err_t espaperplay_reader_history_clear(void) {
    hist_work_t w = {0};
    w.type = HIST_JOB_CLEAR;
    return hist_post(&w);
}

esp_err_t espaperplay_reader_history_flush(uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_hist_pending > 0) {
        if (xTaskGetTickCount() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}
