/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#include "espaperplay_session.h"

static const char *TAG = "ESPaperPlay_SESSION";

/* SHA-256 摘要长度（字节）。 */
#define SESSION_HASH_LEN 32

/**
 * @brief 会话表条目。
 */
typedef struct {
    bool active;                          /*!< 槽位是否被占用 */
    espaperplay_session_id_t id;          /*!< 会话 ID */
    uint8_t token_hash[SESSION_HASH_LEN]; /*!< token 的 SHA-256 哈希 */
    uint64_t last_active_ms;              /*!< 最近活跃时间戳（ms） */
    uint32_t ttl_ms;                      /*!< 有效期（ms） */
} session_entry_t;

static session_entry_t s_sessions[ESPAPERPLAY_SESSION_MAX];
static SemaphoreHandle_t s_mutex;              /*!< 会话表互斥锁 */
static espaperplay_session_id_t s_next_id = 1; /*!< 会话 ID 分配器 */
static esp_timer_handle_t s_cleanup_timer;
static bool s_initialized = false;

/* 登录准入控制状态（内存态，重启清零）。 */
static uint32_t s_login_failures;
static uint64_t s_lockout_until_ms;

/* ------------------------------------------------------------------ */
/* 底层辅助函数                                                         */
/* ------------------------------------------------------------------ */

/** 当前时间（毫秒）。 */
static uint64_t session_now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/** 恒定时间比较，避免时序侧信道泄露比较结果。 */
static uint8_t secure_memcmp(const void *a, const void *b, size_t n) {
    const volatile uint8_t *pa = (const volatile uint8_t *)a;
    const volatile uint8_t *pb = (const volatile uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(pa[i] ^ pb[i]);
    }
    return diff;
}

/** 计算 token 原始字节的 SHA-256 哈希。 */
static esp_err_t hash_token(const uint8_t *raw, size_t len, uint8_t hash[SESSION_HASH_LEN]) {
    size_t hash_len = 0;
    psa_status_t status =
        psa_hash_compute(PSA_ALG_SHA_256, raw, len, hash, SESSION_HASH_LEN, &hash_len);
    if (status != PSA_SUCCESS || hash_len != SESSION_HASH_LEN) {
        ESP_LOGE(TAG, "SHA-256 failed: %d", (int)status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** hex 编码（dst 容量至少 2*len+1）。 */
static void to_hex(const uint8_t *src, size_t len, char *dst) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[i * 2] = digits[src[i] >> 4];
        dst[i * 2 + 1] = digits[src[i] & 0x0f];
    }
    dst[len * 2] = '\0';
}

/** hex 解码；长度或字符非法时返回 false。 */
static bool from_hex(const char *src, size_t hex_len, uint8_t *dst) {
    if (hex_len != ESPAPERPLAY_SESSION_TOKEN_LEN * 2) {
        return false;
    }
    for (size_t i = 0; i < ESPAPERPLAY_SESSION_TOKEN_LEN; i++) {
        char hi = src[i * 2];
        char lo = src[i * 2 + 1];
        uint8_t h = (hi >= '0' && hi <= '9')   ? (uint8_t)(hi - '0')
                    : (hi >= 'a' && hi <= 'f') ? (uint8_t)(hi - 'a' + 10)
                    : (hi >= 'A' && hi <= 'F') ? (uint8_t)(hi - 'A' + 10)
                                               : 0xff;
        uint8_t l = (lo >= '0' && lo <= '9')   ? (uint8_t)(lo - '0')
                    : (lo >= 'a' && lo <= 'f') ? (uint8_t)(lo - 'a' + 10)
                    : (lo >= 'A' && lo <= 'F') ? (uint8_t)(lo - 'A' + 10)
                                               : 0xff;
        if (h == 0xff || l == 0xff) {
            return false;
        }
        dst[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

/**
 * @brief 查找匹配 token 哈希的会话槽位（调用方需持有锁）。
 *
 * 过期会话在此被惰性清理；命中时刷新最近活跃时间（滑动过期）。
 *
 * @return 匹配的槽位下标，未命中返回 -1。
 */
static int session_find(const uint8_t *hash, uint64_t now) {
    for (int i = 0; i < ESPAPERPLAY_SESSION_MAX; i++) {
        if (!s_sessions[i].active) {
            continue;
        }
        if (secure_memcmp(s_sessions[i].token_hash, hash, SESSION_HASH_LEN) != 0) {
            continue;
        }
        if (s_sessions[i].last_active_ms + s_sessions[i].ttl_ms < now) {
            s_sessions[i].active = false; /* 惰性清理过期会话 */
            return -1;
        }
        s_sessions[i].last_active_ms = now;
        return i;
    }
    return -1;
}

/** 清理全部过期会话（调用方需持有锁）。 */
static void cleanup_locked(void) {
    uint64_t now = session_now_ms();
    for (int i = 0; i < ESPAPERPLAY_SESSION_MAX; i++) {
        if (s_sessions[i].active && s_sessions[i].last_active_ms + s_sessions[i].ttl_ms < now) {
            ESP_LOGD(TAG, "Session %lu expired", (unsigned long)s_sessions[i].id);
            s_sessions[i].active = false;
        }
    }
}

/** 周期清理定时器回调。 */
static void session_cleanup_timer_cb(void *arg) {
    (void)arg;
    espaperplay_session_cleanup();
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                            */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_session_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "Session already initialized");
        return ESP_OK;
    }

    /* PSA 密码学库（幂等；ESP-IDF 启动时通常已自动初始化）。 */
    psa_status_t psa_status = psa_crypto_init();
    if (psa_status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)psa_status);
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* 周期清理过期会话。 */
    const esp_timer_create_args_t args = {
        .callback = session_cleanup_timer_cb,
        .name = "session_cleanup",
    };
    esp_err_t err = esp_timer_create(&args, &s_cleanup_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create cleanup timer: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }
    err = esp_timer_start_periodic(s_cleanup_timer,
                                   (uint64_t)ESPAPERPLAY_SESSION_CLEANUP_PERIOD_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start cleanup timer: %s", esp_err_to_name(err));
        esp_timer_delete(s_cleanup_timer);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Session service initialized (max=%d, ttl=%u ms)", ESPAPERPLAY_SESSION_MAX,
             (unsigned)ESPAPERPLAY_SESSION_DEFAULT_TTL_MS);
    return ESP_OK;
}

esp_err_t espaperplay_session_create(uint32_t ttl_ms, char *token_buf, size_t token_buf_size,
                                     espaperplay_session_id_t *out_id) {
    if (token_buf == NULL || token_buf_size < ESPAPERPLAY_SESSION_TOKEN_HEX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ttl_ms == 0) {
        ttl_ms = ESPAPERPLAY_SESSION_DEFAULT_TTL_MS;
    }

    /* 生成随机 token 并计算其哈希。 */
    uint8_t raw[ESPAPERPLAY_SESSION_TOKEN_LEN];
    esp_fill_random(raw, sizeof(raw));
    uint8_t hash[SESSION_HASH_LEN];
    esp_err_t err = hash_token(raw, sizeof(raw), hash);
    if (err != ESP_OK) {
        return err;
    }

    uint64_t now = session_now_ms();
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 优先复用空闲槽位；表满时踢掉最久未活跃的会话。 */
    int slot = -1;
    int oldest = -1;
    uint64_t oldest_active = UINT64_MAX;
    for (int i = 0; i < ESPAPERPLAY_SESSION_MAX; i++) {
        if (!s_sessions[i].active) {
            slot = i;
            break;
        }
        if (s_sessions[i].last_active_ms < oldest_active) {
            oldest_active = s_sessions[i].last_active_ms;
            oldest = i;
        }
    }
    if (slot < 0) {
        slot = oldest;
        ESP_LOGW(TAG, "Session table full, evicting oldest session");
    }

    s_sessions[slot].active = true;
    s_sessions[slot].id = s_next_id++;
    memcpy(s_sessions[slot].token_hash, hash, SESSION_HASH_LEN);
    s_sessions[slot].last_active_ms = now;
    s_sessions[slot].ttl_ms = ttl_ms;
    espaperplay_session_id_t id = s_sessions[slot].id;

    xSemaphoreGive(s_mutex);

    to_hex(raw, sizeof(raw), token_buf);
    if (out_id != NULL) {
        *out_id = id;
    }
    ESP_LOGI(TAG, "Session %lu created", (unsigned long)id);
    return ESP_OK;
}

esp_err_t espaperplay_session_verify(const char *token, espaperplay_session_id_t *out_id) {
    if (token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[ESPAPERPLAY_SESSION_TOKEN_LEN];
    if (!from_hex(token, strlen(token), raw)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hash[SESSION_HASH_LEN];
    esp_err_t err = hash_token(raw, sizeof(raw), hash);
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    int slot = session_find(hash, session_now_ms());
    if (slot < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    espaperplay_session_id_t id = s_sessions[slot].id;
    xSemaphoreGive(s_mutex);

    if (out_id != NULL) {
        *out_id = id;
    }
    return ESP_OK;
}

esp_err_t espaperplay_session_renew(const char *token, uint32_t ttl_ms) {
    if (token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ttl_ms == 0) {
        ttl_ms = ESPAPERPLAY_SESSION_DEFAULT_TTL_MS;
    }

    uint8_t raw[ESPAPERPLAY_SESSION_TOKEN_LEN];
    if (!from_hex(token, strlen(token), raw)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hash[SESSION_HASH_LEN];
    esp_err_t err = hash_token(raw, sizeof(raw), hash);
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    int slot = session_find(hash, session_now_ms());
    if (slot < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    s_sessions[slot].ttl_ms = ttl_ms;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t espaperplay_session_destroy(const char *token) {
    if (token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[ESPAPERPLAY_SESSION_TOKEN_LEN];
    if (!from_hex(token, strlen(token), raw)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hash[SESSION_HASH_LEN];
    esp_err_t err = hash_token(raw, sizeof(raw), hash);
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < ESPAPERPLAY_SESSION_MAX; i++) {
        if (s_sessions[i].active &&
            secure_memcmp(s_sessions[i].token_hash, hash, SESSION_HASH_LEN) == 0) {
            s_sessions[i].active = false;
            ESP_LOGI(TAG, "Session %lu destroyed", (unsigned long)s_sessions[i].id);
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK; /* token 不存在也视为成功（幂等） */
}

esp_err_t espaperplay_session_cleanup(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    cleanup_locked();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

size_t espaperplay_session_count(void) {
    if (!s_initialized) {
        return 0;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    size_t n = 0;
    for (int i = 0; i < ESPAPERPLAY_SESSION_MAX; i++) {
        if (s_sessions[i].active) {
            n++;
        }
    }
    xSemaphoreGive(s_mutex);
    return n;
}

bool espaperplay_session_login_allowed(void) {
    if (!s_initialized) {
        return true; /* 未初始化：视为未锁定 */
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool allowed = (s_lockout_until_ms == 0 || s_lockout_until_ms <= session_now_ms());
    xSemaphoreGive(s_mutex);
    return allowed;
}

esp_err_t espaperplay_session_login_failure(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_login_failures++;
    if (s_login_failures >= ESPAPERPLAY_SESSION_LOGIN_MAX_FAILURES) {
        s_lockout_until_ms = session_now_ms() + ESPAPERPLAY_SESSION_LOGIN_LOCKOUT_MS;
        s_login_failures = 0;
        ESP_LOGW(TAG, "Login lockout for %u ms", (unsigned)ESPAPERPLAY_SESSION_LOGIN_LOCKOUT_MS);
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t espaperplay_session_login_success(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_login_failures = 0;
    s_lockout_until_ms = 0;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
