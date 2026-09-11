/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_diaglog.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_log_write.h"
#include "esp_timer.h"

#include "espaperplay_config.h"

static const char *TAG = "ESPaperPlay_DIAGLOG";

/*!< 单行缓冲上限（超长截断，不允许半行写入）。 */
#define DIAGLOG_LINE_MAX 256

/*!< 行内 tag 字段上限（超长截断；本工程最长 tag 约 24 字符）。 */
#define DIAGLOG_TAG_MAX 24

/*!< 捕获队列深度（存指针；行缓冲按需从 PSRAM 分配）。 */
#define DIAGLOG_QUEUE_LEN 32

/*!< SD 就绪前的预留环形缓冲行数（PSRAM，约 16 * 行项大小）。 */
#define DIAGLOG_RING_LINES 16

/*!< 显式写文件锁超时：拿不到锁宁可丢弃本行，也不阻塞诊断/电源任务。 */
#define DIAGLOG_LOCK_TIMEOUT_MS 100

/*!< 墙钟有效性下限（2025-01-01）：时间尚未同步时不输出误导性日期。 */
#define DIAGLOG_TIME_VALID_TS 1735689600LL

/*!< 落盘任务参数：唯一做 stdio/FATFS 写链路的地方（链路耗栈，须大栈）。 */
#define DIAGLOG_TASK_STACK_SIZE 4096
#define DIAGLOG_TASK_PRIORITY 1

static bool diaglog_emit(char level, const char *tag, const char *msg);
static void diaglog_drain_ring(void);

/** 捕获队列行项（从 PSRAM 堆分配，落盘后释放）。 */
typedef struct {
    char level; /*!< 等级字符（E/W/I/D/V），0 = 显式事件（行内不展示） */
    char tag[DIAGLOG_TAG_MAX];
    char msg[DIAGLOG_LINE_MAX];
} diaglog_item_t;

/* ---- 锁与通道 ---- */
static SemaphoreHandle_t s_file_lock = NULL; /*!< 文件写入互斥（显式/落盘任务/补写共用） */
static SemaphoreHandle_t s_line_lock = NULL; /*!< 行累积 + 环形缓冲互斥（vprintf 钩子） */
static QueueHandle_t s_queue = NULL;         /*!< 捕获行队列（存 diaglog_item_t*） */
static uint8_t s_queue_storage[DIAGLOG_QUEUE_LEN * sizeof(diaglog_item_t *)];
static StaticQueue_t s_queue_cb;
static TaskHandle_t s_flush_task = NULL;

/* ---- 全局捕获状态 ---- */
static vprintf_like_t s_prev_vprintf = NULL; /*!< 原控制台输出（串口行为保持不变） */
static volatile esp_log_level_t s_sd_level = ESP_LOG_WARN; /*!< 捕获最低等级 */
static diaglog_item_t *s_ring = NULL;  /*!< SD 就绪前环形缓冲（PSRAM） */
static size_t s_ring_head = 0;         /*!< 环形缓冲队首下标 */
static size_t s_ring_count = 0;        /*!< 环形缓冲当前行数（s_line_lock 保护） */
static bool s_sd_ready = false;        /*!< 首次写成功后置位：新行不再入环 */
static uint32_t s_dropped = 0;         /*!< 队列满 / 分配失败的丢弃计数（恢复时补记） */

/* ---- 行累积（IDF v2 日志按 前缀/消息/后缀 分多次碎片调用 vprintf） ---- */
static char s_line[DIAGLOG_LINE_MAX + 1];
static size_t s_line_len = 0;
static char s_cur_level = 0;                /*!< 当前记录等级字符（续行继承） */
static char s_cur_tag[DIAGLOG_TAG_MAX];     /*!< 当前记录 tag（续行继承） */

/* ---- 文件状态 ---- */
static bool s_dir_ready = false;          /*!< 目标目录已确认可用的缓存 */
static bool s_unavailable_logged = false; /*!< 「SD 不可用」串口告警只打一次 */
static int s_last_cleanup_date = 0;       /*!< 上次成功清理的日期（YYYYMMDD，0=未清理） */

/* ------------------------------------------------------------------ */
/* 基础工具                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief 等级字符 -> esp_log_level_t（无法识别返回 ESP_LOG_NONE）。
 */
static esp_log_level_t diaglog_char_level(char c) {
    switch (c) {
    case 'E':
        return ESP_LOG_ERROR;
    case 'W':
        return ESP_LOG_WARN;
    case 'I':
        return ESP_LOG_INFO;
    case 'D':
        return ESP_LOG_DEBUG;
    case 'V':
        return ESP_LOG_VERBOSE;
    default:
        return ESP_LOG_NONE;
    }
}

/**
 * @brief 确保目标目录存在（幂等；SD 未挂载时 mkdir 失败，下次再试）。
 */
static void diaglog_ensure_dir(void) {
    if (s_dir_ready) {
        return;
    }
    struct stat st = {0};
    if (stat(ESPAPERPLAY_SYSTEM_SD_DIR, &st) == 0) {
        s_dir_ready = true;
        return;
    }
    if (mkdir(ESPAPERPLAY_SYSTEM_SD_DIR, 0777) == 0) {
        s_dir_ready = true;
    }
}

/**
 * @brief 按墙钟选择当前写入文件：NTP 同步后按天命名，未同步进 boot 文件。
 *        纯函数，不依赖锁。
 */
static void diaglog_pick_path(char *buf, size_t sz) {
    time_t now = 0;
    time(&now);
    if (now >= DIAGLOG_TIME_VALID_TS) {
        struct tm tm_info = {0};
        localtime_r(&now, &tm_info);
        strftime(buf, sz, ESPAPERPLAY_SYSTEM_SD_DIR "/diag-%Y%m%d.log", &tm_info);
    } else {
        snprintf(buf, sz, ESPAPERPLAY_SYSTEM_SD_DIR "/" ESPAPERPLAY_DIAGLOG_BOOT_NAME);
    }
}

/**
 * @brief 把一行写入当前文件（打开-追加-关闭 + 轮转 + SD 就绪补写触发）。
 *
 * @param level 等级字符（0 = 显式事件，行内不展示等级）。
 * @note 调用方必须已持有 s_file_lock。
 */
static bool diaglog_emit(char level, const char *tag, const char *msg) {
    diaglog_ensure_dir();

    char path[64];
    diaglog_pick_path(path, sizeof(path));

    /* 轮转：当前文件超过上限改名 .old 重新开始（旧 .old 直接覆盖）。 */
    struct stat st = {0};
    if (stat(path, &st) == 0 && st.st_size > ESPAPERPLAY_DIAGLOG_MAX_BYTES) {
        char old_path[ sizeof(path) + 4 ];
        snprintf(old_path, sizeof(old_path), "%s.old", path);
        remove(old_path);
        if (rename(path, old_path) != 0) {
            ESP_LOGW(TAG, "rotate %s failed (keep appending)", path);
        }
    }

    FILE *f = fopen(path, "a");
    if (f == NULL) {
        /* SD 未挂载/被拔卡等：静默丢弃，只提示一次，绝不上抛阻塞调用方。 */
        if (!s_unavailable_logged) {
            ESP_LOGW(TAG, "diag log unavailable (SD not mounted?), events dropped");
            s_unavailable_logged = true;
        }
        return false;
    }
    s_unavailable_logged = false;

    /* 行首：墙钟时间（未同步用占位）+ 开机秒数；捕获行在 tag 前带等级字符。 */
    char stamp[32] = "----";
    time_t now = 0;
    time(&now);
    if (now > DIAGLOG_TIME_VALID_TS) {
        struct tm tm_info = {0};
        localtime_r(&now, &tm_info);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_info);
    }
    const int64_t up_us = esp_timer_get_time();

    if (level != 0) {
        fprintf(f, "%s [up %lld.%03lld] %c/%s: %s\n", stamp, (long long)(up_us / 1000000),
                (long long)((up_us % 1000000) / 1000), level, tag, msg);
    } else {
        fprintf(f, "%s [up %lld.%03lld] %s: %s\n", stamp, (long long)(up_us / 1000000),
                (long long)((up_us % 1000000) / 1000), tag, msg);
    }
    fclose(f);

    /* 首次写成功：SD 已就绪，把环形缓冲里的开机现场补写出来。 */
    if (!s_sd_ready) {
        s_sd_ready = true;
        diaglog_drain_ring();
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 环形缓冲（SD 就绪前的开机现场留存）                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 弹出环形缓冲最旧行拷贝到 out（无行返回 false）。须持有 s_line_lock。
 */
static bool diaglog_ring_shift_locked(diaglog_item_t *out) {
    if (s_ring == NULL || s_ring_count == 0) {
        return false;
    }
    *out = s_ring[s_ring_head];
    s_ring_head = (s_ring_head + 1) % DIAGLOG_RING_LINES;
    s_ring_count--;
    return true;
}

/**
 * @brief 从环形缓冲取出最旧一行（拷贝到调用方缓冲，无行返回 false）。
 *
 * 拷贝出锁后再落盘：环形槽位会被新行复用，不能持锁做文件 I/O，
 * 也不能让落盘期间槽位被并发改写。
 */
static bool diaglog_ring_pop_copy(diaglog_item_t *out) {
    xSemaphoreTake(s_line_lock, portMAX_DELAY);
    const bool ok = diaglog_ring_shift_locked(out);
    xSemaphoreGive(s_line_lock);
    return ok;
}

/**
 * @brief 把环形缓冲中的行补写到 SD（SD 就绪后由 diaglog_emit 触发一次）。
 *
 * @note 调用方必须已持有 s_file_lock（diaglog_emit 从写成功路径调用）。
 */
static void diaglog_drain_ring(void) {
    if (s_ring == NULL) {
        return;
    }
    diaglog_item_t local; /*!< 栈上拷贝（约 280B）；调用方栈均 ≥4096 */
    while (diaglog_ring_pop_copy(&local)) {
        (void)diaglog_emit(local.level, local.tag, local.msg);
    }
}

/* ------------------------------------------------------------------ */
/* 捕获通道：vprintf 钩子 -> 行解析 -> 等级过滤 -> 队列                   */
/* ------------------------------------------------------------------ */

/**
 * @brief 解析 esp_log 格式化行：`[ANSI] L (ts) TAG: msg`。
 *
 * IDF v2 无颜色（CONFIG_LOG_COLORS 未开）时形如 "I (12345) TAG: msg"；
 * 仍兼容带 ANSI 前缀的形态。无法识别（多行消息的续行、外来格式）返回 false。
 */
static bool diaglog_parse_line(const char *line, char *level, char *tag, size_t tag_sz,
                               const char **msg) {
    const char *p = line;
    if (p[0] == '\x1b' && p[1] == '[') { /* ANSI 颜色前缀（当前固件关闭，保险兼容） */
        p += 2;
        while (*p != '\0' && !isalpha((unsigned char)*p)) {
            p++;
        }
        if (*p != 'm') {
            return false;
        }
        p++;
    }
    if (p[0] == '\0' || p[1] != ' ' || diaglog_char_level(p[0]) == ESP_LOG_NONE) {
        return false;
    }
    const char *close = strchr(p + 2, ')'); /* 时间戳右括号 "(12345)" */
    if (close == NULL || close[1] != ' ') {
        return false;
    }
    const char *tag_start = close + 2;
    const char *colon = strstr(tag_start, ": ");
    if (colon == NULL) {
        return false;
    }
    size_t len = (size_t)(colon - tag_start);
    if (len >= tag_sz) {
        len = tag_sz - 1;
    }
    memcpy(tag, tag_start, len);
    tag[len] = '\0';
    *level = p[0];
    *msg = colon + 2;
    return true;
}

/**
 * @brief 捕获行入队（SD 未就绪先进环形缓冲）。须持有 s_line_lock。
 */
static void diaglog_capture_enqueue_locked(char level, const char *tag, const char *msg) {
    if (s_queue == NULL) {
        return;
    }
    if (!s_sd_ready && s_ring != NULL) {
        /* SD 就绪前：留存在环形缓冲（满则挤掉最旧行，原地覆盖队首槽位）。 */
        if (s_ring_count == DIAGLOG_RING_LINES) {
            s_ring_head = (s_ring_head + 1) % DIAGLOG_RING_LINES;
            s_ring_count--;
            s_dropped++;
        }
        diaglog_item_t *slot = &s_ring[(s_ring_head + s_ring_count) % DIAGLOG_RING_LINES];
        slot->level = level;
        strlcpy(slot->tag, tag, sizeof(slot->tag));
        strlcpy(slot->msg, msg, sizeof(slot->msg));
        s_ring_count++;
        return;
    }

    if (uxQueueSpacesAvailable(s_queue) == 0) {
        s_dropped++;
        return;
    }
    diaglog_item_t *it = heap_caps_malloc(sizeof(diaglog_item_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (it == NULL) {
        it = heap_caps_malloc(sizeof(diaglog_item_t), MALLOC_CAP_8BIT);
    }
    if (it == NULL) {
        s_dropped++;
        return;
    }
    it->level = level;
    strlcpy(it->tag, tag, sizeof(it->tag));
    strlcpy(it->msg, msg, sizeof(it->msg));
    /* 恢复后把丢弃数量补记在本行行尾，避免丢行无迹可循。 */
    if (s_dropped > 0) {
        size_t len = strlen(it->msg);
        snprintf(it->msg + len, sizeof(it->msg) - len, " [+%lu dropped]", (unsigned long)s_dropped);
        s_dropped = 0;
    }
    if (xQueueSend(s_queue, &it, 0) != pdTRUE) {
        free(it);
        s_dropped++;
    }
}

/**
 * @brief 处理一条完整行：解析 / 继承等级与 tag，过滤后入队。须持有 s_line_lock。
 */
static void diaglog_process_line_locked(char *line) {
    char level = 0;
    char tag[DIAGLOG_TAG_MAX];
    const char *msg = NULL;
    if (diaglog_parse_line(line, &level, tag, sizeof(tag), &msg)) {
        /* 记录本条等级与 tag，供多行消息的续行继承。 */
        s_cur_level = level;
        strlcpy(s_cur_tag, tag, sizeof(s_cur_tag));
    } else {
        level = s_cur_level;
        strlcpy(tag, s_cur_tag, sizeof(tag));
        msg = line;
    }
    if (level == 0) {
        return; /* 无法归类的行（首个片段即非日志格式），只留串口 */
    }
    if (s_sd_level == ESP_LOG_NONE || diaglog_char_level(level) < s_sd_level) {
        return; /* 低于配置等级：过滤（显式事件不走此路径，不受影响） */
    }
    diaglog_capture_enqueue_locked(level, tag, msg);
}

/**
 * @brief esp_log 输出接管（esp_log_set_vprintf）。
 *
 * IDF v2 日志把每条记录拆成 前缀("I (123) TAG: ") / 消息 / 后缀("\n") 多个
 * 碎片调用本函数：先原样转发给原控制台输出（串口行为不变），再把碎片累积
 * 成完整行后解析过滤入队。记录级串行化由日志库的 flockfile(stdout) 保证，
 * 这里再以 s_line_lock 兜底（拿不到锁只丢捕获，不阻塞日志调用方）。
 */
static int diaglog_log_hook(const char *format, va_list args) {
    if (s_prev_vprintf != NULL) {
        va_list copy;
        va_copy(copy, args);
        s_prev_vprintf(format, copy);
        va_end(copy);
    }
    if (s_line_lock == NULL || xPortInIsrContext()) {
        return 0; /* 日志库不支持 ISR 调用；防御性直通 */
    }

    char frag[DIAGLOG_LINE_MAX];
    int ret = vsnprintf(frag, sizeof(frag), format, args);

    if (xSemaphoreTake(s_line_lock, 0) != pdTRUE) {
        return ret; /* 竞争即弃本碎片（flockfile 下几乎不发生） */
    }
    for (const char *p = frag; *p != '\0'; p++) {
        if (*p == '\n' || s_line_len >= DIAGLOG_LINE_MAX) {
            if (s_line_len > 0) { /* 空行（消息自带换行产生的连续换行）不落盘 */
                s_line[s_line_len] = '\0';
                diaglog_process_line_locked(s_line);
                s_line_len = 0;
            }
            if (*p != '\n') {
                s_line[s_line_len++] = *p;
            }
        } else {
            s_line[s_line_len++] = *p;
        }
    }
    xSemaphoreGive(s_line_lock);
    return ret;
}

/* ------------------------------------------------------------------ */
/* 落盘任务与定期清理                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 定期清理旧日志：按文件名日期删超过保留期的 diag-*.log/.old，
 *        boot 文件按 mtime 判定。仅由落盘任务调用（在开机与日期切换时生效）。
 */
static void diaglog_cleanup_old(void) {
    time_t now = 0;
    time(&now);
    if (now < DIAGLOG_TIME_VALID_TS) {
        return; /* 时间未同步：无法判定新旧，绝不误删 */
    }
    struct tm tm_info = {0};
    time_t cutoff = now - (time_t)ESPAPERPLAY_DIAGLOG_RETENTION_DAYS * 86400;
    char cutoff_str[9];
    localtime_r(&cutoff, &tm_info);
    strftime(cutoff_str, sizeof(cutoff_str), "%Y%m%d", &tm_info);

    DIR *d = opendir(ESPAPERPLAY_SYSTEM_SD_DIR);
    if (d == NULL) {
        return; /* SD 未就绪：不推进清理游标，下次再试 */
    }
    int removed = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        bool dated = strncmp(n, "diag-", 5) == 0 && strlen(n) >= 5 + 8;
        bool digits = dated;
        for (int i = 5; digits && i < 13; i++) {
            digits = isdigit((unsigned char)n[i]);
        }
        if (digits && strncmp(n + 5, cutoff_str, 8) < 0) {
            char path[64];
            snprintf(path, sizeof(path), ESPAPERPLAY_SYSTEM_SD_DIR "/%s", n);
            if (unlink(path) == 0) {
                removed++;
            }
        } else if (strcmp(n, ESPAPERPLAY_DIAGLOG_BOOT_NAME) == 0) {
            char path[64];
            struct stat st = {0};
            snprintf(path, sizeof(path), ESPAPERPLAY_SYSTEM_SD_DIR "/%s", n);
            if (stat(path, &st) == 0 && (time_t)st.st_mtime < cutoff) {
                if (unlink(path) == 0) {
                    removed++;
                }
            }
        }
    }
    closedir(d);
    /* 清理游标只在目录成功扫描后推进，明天（或日期变化时）再清一轮。 */
    struct tm today_tm = {0};
    localtime_r(&now, &today_tm);
    s_last_cleanup_date =
        (today_tm.tm_year + 1900) * 10000 + (today_tm.tm_mon + 1) * 100 + today_tm.tm_mday;
    if (removed > 0) {
        ESP_LOGI(TAG, "cleaned %d diag files older than %s", removed, cutoff_str);
    }
}

/** 今天是否已清理过（避免每批行都扫目录）。 */
static bool diaglog_cleanup_done_today(void) {
    time_t now = 0;
    time(&now);
    if (now < DIAGLOG_TIME_VALID_TS) {
        return true; /* 时间未同步时无需清理 */
    }
    struct tm tm_info = {0};
    localtime_r(&now, &tm_info);
    const int today =
        (tm_info.tm_year + 1900) * 10000 + (tm_info.tm_mon + 1) * 100 + tm_info.tm_mday;
    return today == s_last_cleanup_date;
}

/**
 * @brief 落盘任务：阻塞等捕获队列，逐批写盘（每批一次拿锁），顺带做定期清理。
 */
static void diaglog_flush_task_fn(void *arg) {
    (void)arg;
    diaglog_item_t *it = NULL;
    for (;;) {
        if (xQueueReceive(s_queue, &it, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(DIAGLOG_LOCK_TIMEOUT_MS)) != pdTRUE) {
            free(it); /* 显式事件正长写：放弃本批，后续行照常 */
            continue;
        }
        do {
            (void)diaglog_emit(it->level, it->tag, it->msg);
            free(it);
        } while (xQueueReceive(s_queue, &it, 0) == pdTRUE);
        xSemaphoreGive(s_file_lock);

        if (!diaglog_cleanup_done_today()) {
            if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
                diaglog_cleanup_old();
                xSemaphoreGive(s_file_lock);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                             */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_diaglog_init(void) {
    if (s_file_lock != NULL) {
        return ESP_OK;
    }
    s_file_lock = xSemaphoreCreateMutex();
    s_line_lock = xSemaphoreCreateMutex();
    if (s_file_lock == NULL || s_line_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_queue = xQueueCreateStatic(DIAGLOG_QUEUE_LEN, sizeof(diaglog_item_t *), s_queue_storage,
                                 &s_queue_cb);
    s_ring = heap_caps_calloc(DIAGLOG_RING_LINES, sizeof(diaglog_item_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_queue == NULL || s_ring == NULL) {
        if (s_ring != NULL) {
            free(s_ring);
            s_ring = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(diaglog_flush_task_fn, "diaglog", DIAGLOG_TASK_STACK_SIZE, NULL,
                    DIAGLOG_TASK_PRIORITY, &s_flush_task) != pdPASS) {
        free(s_ring);
        s_ring = NULL;
        ESP_LOGE(TAG, "create flush task failed (capture disabled, explicit writes keep working)");
        return ESP_ERR_NO_MEM;
    }

    /* 队列与任务就绪后再接管输出：此前的日志只留串口。 */
    s_prev_vprintf = esp_log_set_vprintf(diaglog_log_hook);
    esp_log_level_t level = s_sd_level;
    ESP_LOGI(TAG, "SD log capture ready: %s/diag-YYYYMMDD.log (level>=%d, keep %d days)",
             ESPAPERPLAY_SYSTEM_SD_DIR, (int)level, ESPAPERPLAY_DIAGLOG_RETENTION_DAYS);
    return ESP_OK;
}

bool espaperplay_diaglog_write(const char *tag, const char *fmt, ...) {
    if (s_file_lock == NULL) {
        return false;
    }
    char msg[DIAGLOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(DIAGLOG_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    const bool ok = diaglog_emit(0, tag, msg);
    xSemaphoreGive(s_file_lock);
    return ok;
}

void espaperplay_diaglog_set_level(esp_log_level_t level) {
    if (level < ESP_LOG_NONE || level > ESP_LOG_VERBOSE) {
        return;
    }
    s_sd_level = level;
}

esp_log_level_t espaperplay_diaglog_get_level(void) { return s_sd_level; }
