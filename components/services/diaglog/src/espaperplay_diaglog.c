/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "espaperplay_diaglog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "espaperplay_config.h"

static const char *TAG = "ESPaperPlay_DIAGLOG";

/*!< 单文件上限：超过后轮转为 .old（总占用上限 = 2 倍该值）。 */
#define DIAGLOG_MAX_BYTES (1024 * 1024)

/*!< 单行缓冲上限（超长截断，不允许半行写入）。 */
#define DIAGLOG_LINE_MAX 256

/*!< 锁获取超时：拿不到锁宁可丢弃本行，也不阻塞诊断/电源任务。 */
#define DIAGLOG_LOCK_TIMEOUT_MS 100

static SemaphoreHandle_t s_lock = NULL;        /*!< 写入互斥锁（init 创建） */
static bool s_dir_ready = false;               /*!< 目标目录已确认可用的缓存 */
static bool s_unavailable_logged = false;      /*!< 「SD 不可用」串口告警只打一次 */

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

esp_err_t espaperplay_diaglog_init(void) {
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "create write mutex failed");
        return ESP_ERR_NO_MEM;
    }
    diaglog_ensure_dir();
    ESP_LOGI(TAG, "diag log ready: %s", ESPAPERPLAY_DIAGLOG_PATH);
    return ESP_OK;
}

bool espaperplay_diaglog_write(const char *tag, const char *fmt, ...) {
    if (s_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(DIAGLOG_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }

    char msg[DIAGLOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* 行首：墙钟时间（NTP 同步前为 1970 纪元，用占位显示）+ 开机秒数。 */
    char stamp[32] = "----";
    time_t now = 0;
    time(&now);
    if (now > 1735689600) { /* 2025-01-01：时间尚未同步时不输出误导性日期 */
        struct tm tm_info = {0};
        localtime_r(&now, &tm_info);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_info);
    }
    const int64_t up_us = esp_timer_get_time();

    diaglog_ensure_dir();

    /* 轮转：超过上限把当前文件改名 .old 重新开始（旧 .old 直接覆盖）。 */
    struct stat st = {0};
    if (stat(ESPAPERPLAY_DIAGLOG_PATH, &st) == 0 && st.st_size > DIAGLOG_MAX_BYTES) {
        const char *old_path = ESPAPERPLAY_DIAGLOG_PATH ".old";
        remove(old_path);
        if (rename(ESPAPERPLAY_DIAGLOG_PATH, old_path) != 0) {
            ESP_LOGW(TAG, "rotate %s failed (keep appending)", ESPAPERPLAY_DIAGLOG_PATH);
        }
    }

    FILE *f = fopen(ESPAPERPLAY_DIAGLOG_PATH, "a");
    if (f == NULL) {
        /* SD 未挂载/被拔卡等：静默丢弃，只提示一次，绝不上抛阻塞调用方。 */
        if (!s_unavailable_logged) {
            ESP_LOGW(TAG, "diag log unavailable (SD not mounted?), events dropped");
            s_unavailable_logged = true;
        }
        xSemaphoreGive(s_lock);
        return false;
    }
    s_unavailable_logged = false;

    fprintf(f, "%s [up %lld.%03lld] %s: %s\n", stamp, (long long)(up_us / 1000000),
            (long long)((up_us % 1000000) / 1000), tag, msg);
    fclose(f);

    xSemaphoreGive(s_lock);
    return true;
}
