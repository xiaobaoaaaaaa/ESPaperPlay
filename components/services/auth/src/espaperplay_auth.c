/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

#include "espaperplay_auth.h"

static const char *TAG = "ESPaperPlay_AUTH";

/* NVS 键名。 */
#define NVS_KEY_PASSWORD "pwd" /*!< 密码记录（blob） */

/**
 * @brief NVS 中密码记录的二进制布局（packed，共 53 字节）。
 *
 * 版本号便于未来升级格式；迭代次数随记录保存，即使默认迭代次数调整，
 * 旧记录仍可正确校验（用记录内的参数而非当前宏值）。
 */
typedef struct __attribute__((packed)) {
    uint8_t version;                               /*!< 格式版本 */
    uint32_t iterations;                           /*!< PBKDF2 迭代次数 */
    uint8_t salt[ESPAPERPLAY_AUTH_SALT_LEN];       /*!< 随机盐 */
    uint8_t derived[ESPAPERPLAY_AUTH_DERIVED_LEN]; /*!< PBKDF2-HMAC-SHA256 派生值 */
} espaperplay_auth_blob_t;

/* 内存缓存：初始化时从 NVS 加载，更改时同步更新。 */
static espaperplay_auth_blob_t s_blob;
static bool s_initialized = false;
static bool s_configured = false;

/* ------------------------------------------------------------------ */
/* 底层辅助函数                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief 恒定时间比较，避免时序侧信道泄露比较结果。
 *
 * @return 相等返回 0，否则非 0（不泄露差异位置）。
 */
static uint8_t secure_memcmp(const void *a, const void *b, size_t n) {
    const volatile uint8_t *pa = (const volatile uint8_t *)a;
    const volatile uint8_t *pb = (const volatile uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(pa[i] ^ pb[i]);
    }
    return diff;
}

/** 清零敏感缓冲区（防编译器优化掉）。 */
static void secure_zeroize(void *buf, size_t n) {
    volatile uint8_t *p = (volatile uint8_t *)buf;
    while (n--) {
        *p++ = 0;
    }
}

/**
 * @brief 使用 PBKDF2-HMAC-SHA256 从密码派生密钥。
 *
 * 使用 PSA 标准 key derivation 实现，输入顺序固定为
 * COST（迭代次数）→ SALT → PASSWORD。
 *
 * @param iterations PBKDF2 迭代次数。
 * @param salt       随机盐（ESPAPERPLAY_AUTH_SALT_LEN 字节）。
 * @param password   密码明文（以 strlen 长度参与哈希，不含 '\0'）。
 * @param out        输出缓冲（ESPAPERPLAY_AUTH_DERIVED_LEN 字节）。
 * @return 成功返回 ESP_OK，失败返回 ESP_FAIL。
 */
static esp_err_t auth_derive(uint32_t iterations, const uint8_t *salt, const char *password,
                             uint8_t *out) {
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iterations);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt,
                                                ESPAPERPLAY_AUTH_SALT_LEN);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                                (const uint8_t *)password, strlen(password));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&op, out, ESPAPERPLAY_AUTH_DERIVED_LEN);
    }
    psa_key_derivation_abort(&op);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PBKDF2 derive failed: %d", (int)status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 生成密码记录：随机盐 + 按当前迭代次数派生。
 */
static esp_err_t auth_make_blob(const char *password, espaperplay_auth_blob_t *blob) {
    memset(blob, 0, sizeof(*blob));
    esp_fill_random(blob->salt, sizeof(blob->salt));
    blob->version = ESPAPERPLAY_AUTH_BLOB_VERSION;
    blob->iterations = ESPAPERPLAY_AUTH_PBKDF2_ITERATIONS;
    return auth_derive(blob->iterations, blob->salt, password, blob->derived);
}

/**
 * @brief 从 NVS 加载密码记录与默认标记，刷新内存缓存。
 */
static esp_err_t auth_load(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPAPERPLAY_AUTH_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* 命名空间尚不存在：视为未配置（出厂无密码状态）。 */
        s_configured = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(s_blob);
    err = nvs_get_blob(handle, NVS_KEY_PASSWORD, &s_blob, &len);
    if (err == ESP_OK && len == sizeof(s_blob) && s_blob.version == ESPAPERPLAY_AUTH_BLOB_VERSION) {
        s_configured = true;
    } else {
        s_configured = false;
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Password record missing or invalid: %s", esp_err_to_name(err));
        }
    }

    nvs_close(handle);
    return ESP_OK;
}

/**
 * @brief 将密码记录写回 NVS 并刷新内存缓存。
 */
static esp_err_t auth_save(const espaperplay_auth_blob_t *blob) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPAPERPLAY_AUTH_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY_PASSWORD, blob, sizeof(*blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        s_blob = *blob;
        s_configured = true;
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                            */
/* ------------------------------------------------------------------ */

esp_err_t espaperplay_auth_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "Auth already initialized");
        return ESP_OK;
    }

    /* 初始化 NVS 分区；分区满或格式版本变化时先擦除重建。 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 初始化 PSA 密码学库（幂等；ESP-IDF 启动时通常已自动初始化）。 */
    psa_status_t psa_status = psa_crypto_init();
    if (psa_status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)psa_status);
        return ESP_ERR_INVALID_STATE;
    }

    err = auth_load();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load auth record: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Auth initialized, password %s",
             s_configured ? "configured" : "NOT set (unconfigured, first-time setup required)");
    return ESP_OK;
}

bool espaperplay_auth_is_configured(void) { return s_configured; }

esp_err_t espaperplay_auth_verify(const char *password) {
    if (password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(password);
    if (len < ESPAPERPLAY_AUTH_PASSWORD_MIN_LEN || len >= ESPAPERPLAY_AUTH_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_configured) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 用记录内的盐与迭代次数重新派生输入密码，恒定时间比较。 */
    uint8_t derived[ESPAPERPLAY_AUTH_DERIVED_LEN];
    esp_err_t err = auth_derive(s_blob.iterations, s_blob.salt, password, derived);
    if (err != ESP_OK) {
        secure_zeroize(derived, sizeof(derived));
        return err;
    }
    uint8_t mismatch = secure_memcmp(derived, s_blob.derived, sizeof(derived));
    secure_zeroize(derived, sizeof(derived));

    return mismatch == 0 ? ESP_OK : ESP_ERR_NOT_ALLOWED;
}

esp_err_t espaperplay_auth_change_password(const char *new_password) {
    if (new_password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(new_password);
    if (len < ESPAPERPLAY_AUTH_PASSWORD_MIN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len >= ESPAPERPLAY_AUTH_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    espaperplay_auth_blob_t blob;
    esp_err_t err = auth_make_blob(new_password, &blob);
    if (err != ESP_OK) {
        return err;
    }
    err = auth_save(&blob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist new password: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Password changed");
    return ESP_OK;
}

esp_err_t espaperplay_auth_clear_password(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPAPERPLAY_AUTH_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_erase_key(handle, NVS_KEY_PASSWORD);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* 本就没有密码，视为成功 */
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        s_configured = false;
        memset(&s_blob, 0, sizeof(s_blob));
        ESP_LOGW(TAG, "Password cleared, auth unconfigured");
    }
    return err;
}
