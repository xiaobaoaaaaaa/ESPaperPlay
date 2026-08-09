/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/asn1.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

#include "espaperplay_wifi.h"
#include "webserver_internal.h"

static const char *TAG = "ESPaperPlay_WEB";

/* NVS 命名空间与键名。 */
#define TLS_NVS_NAMESPACE "tls" /*!< 证书/私钥存储命名空间 */
#define TLS_NVS_KEY_CERT "cert" /*!< 证书（DER） */
#define TLS_NVS_KEY_KEY "key"   /*!< 私钥（SEC1 DER） */
#define TLS_NVS_KEY_IP "ip"     /*!< 证书生成时记录的 IP（用于检测 IP 变化） */

/* AP 热点默认 IP（证书 SAN 固定项）。 */
#define TLS_SAN_AP_IP_0 192
#define TLS_SAN_AP_IP_1 168
#define TLS_SAN_AP_IP_2 4
#define TLS_SAN_AP_IP_3 1
/* 证书 SAN 中的固定域名（配合 mDNS 或 hosts 映射使用）。 */
#define TLS_SAN_DNS_NAME "espaperplay.local"

/* 自签名证书有效期限（UTC 时间戳，10 年）。 */
#define TLS_CERT_NOT_BEFORE "20260101000000" /*!< 生效时间 */
#define TLS_CERT_NOT_AFTER "20351231235959"  /*!< 过期时间 */

/* 内存缓存：服务器生命周期内证书/私钥持续有效，仅在首次访问时加载/生成一次。 */
static uint8_t *s_cert = NULL;
static size_t s_cert_len = 0;
static uint8_t *s_key = NULL;
static size_t s_key_len = 0;

/* ------------------------------------------------------------------ */
/* 底层辅助                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief 在设备端生成 P-256 密钥对与自签名 X.509 证书。
 *
 * 私钥由设备在首次启动时自行生成，除写入 NVS 外从不出设备；固件镜像中
 * 不包含私钥，即使固件被提取也无法仿冒本设备。
 *
 * @param[in]  san_ip      证书 SAN 中要包含的当前 WiFi IPv4 字符串（可为 NULL/无效，
 *                         表示未知 IP，仅含 AP 固定 IP 与域名）。
 * @param[out] out_cert     证书 DER 数据（堆上分配，调用方负责释放）。
 * @param[out] out_cert_len 证书长度。
 * @param[out] out_key      私钥 SEC1 DER 数据（堆上分配，调用方负责释放）。
 * @param[out] out_key_len  私钥长度。
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
static esp_err_t tls_generate(const char *san_ip, uint8_t **out_cert, size_t *out_cert_len,
                              uint8_t **out_key, size_t *out_key_len) {
    /* 1. 用 PSA 生成 P-256 密钥对并导出私钥标量。
     *    mbedtls 4.x 已移除 mbedtls_pk_gen_key，生成密钥须走 PSA。注意
     *    psa_export_key 对 ECC 私钥返回的是**裸标量**（mbedtls_ecp_write_key_ext
     *    的大端 d，P-256 为 32 字节），并非 DER SEC1。 */
    psa_key_id_t key_id = 0;
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attrs, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs, 256);

    psa_status_t st = psa_generate_key(&attrs, &key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_key failed: %d", (int)st);
        return ESP_FAIL;
    }

    uint8_t d[32];
    size_t d_len = 0;
    st = psa_export_key(key_id, d, sizeof(d), &d_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS || d_len != sizeof(d)) {
        ESP_LOGE(TAG, "psa_export_key failed: %d (len %u)", (int)st, (unsigned)d_len);
        return ESP_FAIL;
    }

    /* 2. 把裸标量组装成带 NamedCurve OID 的 SEC1 (RFC 5915) DER：
     *    SEQUENCE { INTEGER 1, OCTET STRING d, [0] { OID secp256r1 } }。
     *    解析时据此设置曲线 group，mbedtls_pk_parse_key 才能识别。
     *    该 SEC1 也作为私钥持久化 / 交给 esp_tls（握手解析同一格式）。
     *    注意：mbedtls 4.x 的 mbedtls_asn1_write_oid 接收**原始字节**（写
     *    raw buffer），不再像 3.x 那样接受 hex 字符串。 */
    uint8_t sec1[64];
    unsigned char *p = sec1 + sizeof(sec1);
    size_t sec1_len = 0;
    int ret;
    /* secp256r1 = 1.2.840.10045.3.1.7 的 DER 编码（8 字节）。 */
    const unsigned char secp256r1_oid[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
    MBEDTLS_ASN1_CHK_ADD(sec1_len, mbedtls_asn1_write_oid(&p, sec1, (const char *)secp256r1_oid,
                                                          sizeof(secp256r1_oid)));
    MBEDTLS_ASN1_CHK_ADD(sec1_len, mbedtls_asn1_write_len(&p, sec1, sec1_len));
    MBEDTLS_ASN1_CHK_ADD(sec1_len, mbedtls_asn1_write_tag(&p, sec1,
                                                          MBEDTLS_ASN1_CONTEXT_SPECIFIC |
                                                              MBEDTLS_ASN1_CONSTRUCTED | 0));
    MBEDTLS_ASN1_CHK_ADD(sec1_len, mbedtls_asn1_write_octet_string(&p, sec1, d, d_len));
    MBEDTLS_ASN1_CHK_ADD(sec1_len, mbedtls_asn1_write_int(&p, sec1, 1));
    MBEDTLS_ASN1_CHK_ADD(sec1_len, mbedtls_asn1_write_len(&p, sec1, sec1_len));
    MBEDTLS_ASN1_CHK_ADD(sec1_len, mbedtls_asn1_write_tag(
                                       &p, sec1, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));

    /* 3. 解析私钥到 mbedtls_pk_context，供自签名签名使用。 */
    /* 诊断：打印组装后的 SEC1 头，并复刻 mbedtls_pk_ecc_set_key 的 PSA 导入
     * 以确认 set_key 阶段正常（d 内容在 SEC1 偏移 7）。 */
    ESP_LOGI(TAG, "SEC1 len=%u head=%02X%02X%02X%02X%02X%02X%02X%02X", (unsigned)sec1_len, p[0],
             p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

    psa_key_attributes_t imp_attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&imp_attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_usage_flags(&imp_attrs, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&imp_attrs, PSA_ALG_ECDSA(PSA_ALG_ANY_HASH));
    psa_key_id_t test_id = 0;
    psa_status_t imp_st = psa_import_key(&imp_attrs, p + 7, 32, &test_id);
    ESP_LOGI(TAG, "psa_import_key(raw d): %d", (int)imp_st);
    if (imp_st == PSA_SUCCESS) {
        psa_destroy_key(test_id);
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    ret = mbedtls_pk_parse_key(&pk, p, sec1_len, NULL, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_pk_parse_key failed: -0x%04X", -ret);
        mbedtls_pk_free(&pk);
        return ESP_FAIL;
    }

    /* 4. 组装自签名证书：CN 带 MAC 尾缀，便于区分多台设备。 */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char subject[64];
    snprintf(subject, sizeof(subject), "CN=ESPaperPlay-%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);

    uint8_t serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7F; /* 保证序列号为正整数 */

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    mbedtls_x509write_crt_set_subject_name(&crt, subject);
    mbedtls_x509write_crt_set_issuer_name(&crt, subject);
    mbedtls_x509write_crt_set_validity(&crt, TLS_CERT_NOT_BEFORE, TLS_CERT_NOT_AFTER);
    mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));

    /* SubjectAltName：AP 固定 IP + 当前 WiFi IP（若有效）+ 固定域名。
     * 现代浏览器对 https://<IP> 访问要求证书含匹配的 IP SAN，否则可能直接拒绝。 */
    uint8_t ap_ip[4] = {TLS_SAN_AP_IP_0, TLS_SAN_AP_IP_1, TLS_SAN_AP_IP_2, TLS_SAN_AP_IP_3};
    uint8_t cur_ip[4] = {0, 0, 0, 0};
    bool have_cur = false;
    if (san_ip != NULL) {
        unsigned a, b, c, d;
        if (sscanf(san_ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a <= 255 && b <= 255 &&
            c <= 255 && d <= 255) {
            cur_ip[0] = (uint8_t)a;
            cur_ip[1] = (uint8_t)b;
            cur_ip[2] = (uint8_t)c;
            cur_ip[3] = (uint8_t)d;
            have_cur = true;
        }
    }

    mbedtls_x509_san_list san[3] = {0};
    int san_n = 0;
    san[san_n].node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san[san_n].node.san.unstructured_name.p = (unsigned char *)TLS_SAN_DNS_NAME;
    san[san_n].node.san.unstructured_name.len = sizeof(TLS_SAN_DNS_NAME) - 1;
    san_n++;
    san[san_n].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
    san[san_n].node.san.unstructured_name.p = ap_ip;
    san[san_n].node.san.unstructured_name.len = sizeof(ap_ip);
    san_n++;
    if (have_cur && memcmp(cur_ip, ap_ip, sizeof(cur_ip)) != 0) {
        san[san_n].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        san[san_n].node.san.unstructured_name.p = cur_ip;
        san[san_n].node.san.unstructured_name.len = sizeof(cur_ip);
        san_n++;
    }
    for (int i = 0; i < san_n; i++) {
        san[i].next = (i + 1 < san_n) ? &san[i + 1] : NULL;
    }
    mbedtls_x509write_crt_set_subject_alternative_name(&crt, san);

    uint8_t cert_der[2048];
    int cert_der_len = mbedtls_x509write_crt_der(&crt, cert_der, sizeof(cert_der));
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    if (cert_der_len < 0) {
        ESP_LOGE(TAG, "mbedtls_x509write_crt_der failed: -0x%04X", -cert_der_len);
        return ESP_FAIL;
    }
    /* DER 数据从 buffer 末尾写起，移到开头。 */
    memmove(cert_der, cert_der + sizeof(cert_der) - (size_t)cert_der_len, (size_t)cert_der_len);

    /* 4. 拷贝到堆上返回（私钥为组装好的 SEC1 DER，可直接被 esp_tls 解析）。 */
    uint8_t *cert = malloc((size_t)cert_der_len);
    uint8_t *key = malloc(sec1_len);
    if (cert == NULL || key == NULL) {
        free(cert);
        free(key);
        return ESP_ERR_NO_MEM;
    }
    memcpy(cert, cert_der, (size_t)cert_der_len);
    memcpy(key, p, sec1_len);

    *out_cert = cert;
    *out_cert_len = (size_t)cert_der_len;
    *out_key = key;
    *out_key_len = sec1_len;
    return ESP_OK;
}

/**
 * @brief 从 NVS 加载证书/私钥到内存缓存。
 *
 * @return 成功返回 ESP_OK；命名空间或键不存在返回 ESP_ERR_NVS_NOT_FOUND
 *         （由调用方触发首次生成），其余错误原样返回。
 */
static esp_err_t tls_load_from_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TLS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t cert_len = 0, key_len = 0;
    err = nvs_get_blob(handle, TLS_NVS_KEY_CERT, NULL, &cert_len);
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, TLS_NVS_KEY_KEY, NULL, &key_len);
    }

    if (err == ESP_OK) {
        uint8_t *cert = malloc(cert_len);
        uint8_t *key = malloc(key_len);
        if (cert == NULL || key == NULL) {
            free(cert);
            free(key);
            nvs_close(handle);
            return ESP_ERR_NO_MEM;
        }
        if (nvs_get_blob(handle, TLS_NVS_KEY_CERT, cert, &cert_len) != ESP_OK ||
            nvs_get_blob(handle, TLS_NVS_KEY_KEY, key, &key_len) != ESP_OK) {
            free(cert);
            free(key);
            nvs_close(handle);
            return ESP_ERR_NVS_NOT_FOUND;
        }
        s_cert = cert;
        s_cert_len = cert_len;
        s_key = key;
        s_key_len = key_len;
    }
    nvs_close(handle);
    return err;
}

/** 将证书/私钥写入 NVS 持久化，保证重启后证书指纹不变。 */
static esp_err_t tls_save_to_nvs(const uint8_t *cert, size_t cert_len, const uint8_t *key,
                                 size_t key_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TLS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, TLS_NVS_KEY_CERT, cert, cert_len);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, TLS_NVS_KEY_KEY, key, key_len);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/** 打印证书 SHA-256 指纹，便于用户在浏览器端核对连接真实性。 */
static void tls_log_fingerprint(const uint8_t *cert, size_t cert_len) {
    uint8_t digest[32];
    size_t digest_len = 0;
    psa_status_t st =
        psa_hash_compute(PSA_ALG_SHA_256, cert, cert_len, digest, sizeof(digest), &digest_len);
    if (st != PSA_SUCCESS) {
        return;
    }
    char hex[sizeof(digest) * 2 + 1];
    for (size_t i = 0; i < digest_len; i++) {
        snprintf(hex + i * 2, 3, "%02X", digest[i]);
    }
    ESP_LOGI(TAG, "Self-signed cert SHA-256: %s", hex);
}

/* ------------------------------------------------------------------ */
/* IP 记录辅助（用于在 WiFi IP 变化时重新生成证书）                      */
/* ------------------------------------------------------------------ */

/** 读取证书生成时记录的 IP；无记录返回 ESP_ERR_NVS_NOT_FOUND。 */
static esp_err_t tls_get_gen_ip(char *out, size_t out_size) {
    nvs_handle_t handle;
    if (nvs_open(TLS_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t len = out_size;
    esp_err_t err = nvs_get_str(handle, TLS_NVS_KEY_IP, out, &len);
    nvs_close(handle);
    return err;
}

/** 记录证书生成时的 IP。 */
static esp_err_t tls_set_gen_ip(const char *ip) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TLS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, TLS_NVS_KEY_IP, ip);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/** 获取当前有效 WiFi IPv4 字符串；无有效 IP 时输出 "0.0.0.0"。 */
static void tls_get_current_ip(char *out, size_t out_size) {
    snprintf(out, out_size, "0.0.0.0");
    espaperplay_wifi_status_t st;
    if (espaperplay_wifi_get_status(&st) == ESP_OK && st.ip[0] != '\0' &&
        strcmp(st.ip, "0.0.0.0") != 0) {
        snprintf(out, out_size, "%s", st.ip);
    }
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t webserver_tls_get(const uint8_t **cert, size_t *cert_len, const uint8_t **key,
                            size_t *key_len) {
    if (cert == NULL || cert_len == NULL || key == NULL || key_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cert != NULL && s_key != NULL) {
        *cert = s_cert;
        *cert_len = s_cert_len;
        *key = s_key;
        *key_len = s_key_len;
        return ESP_OK;
    }

    /* 初始化 PSA 密码学库（幂等；一般已被 auth/session 组件初始化）。 */
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = tls_load_from_nvs();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* 首次启动：生成并持久化，之后重启加载同一证书（指纹不变）。
         * SAN 尽量包含当前 WiFi IP，便于浏览器 https://<IP> 名称匹配。 */
        uint8_t *new_cert = NULL, *new_key = NULL;
        size_t new_cert_len = 0, new_key_len = 0;
        char ip[16];
        tls_get_current_ip(ip, sizeof(ip));
        err = tls_generate(ip, &new_cert, &new_cert_len, &new_key, &new_key_len);
        if (err != ESP_OK) {
            return err;
        }
        err = tls_save_to_nvs(new_cert, new_cert_len, new_key, new_key_len);
        if (err != ESP_OK) {
            free(new_cert);
            free(new_key);
            ESP_LOGE(TAG, "Failed to persist TLS key material: %s", esp_err_to_name(err));
            return err;
        }
        tls_set_gen_ip(ip);
        s_cert = new_cert;
        s_cert_len = new_cert_len;
        s_key = new_key;
        s_key_len = new_key_len;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load TLS key material: %s", esp_err_to_name(err));
        return err;
    }

    *cert = s_cert;
    *cert_len = s_cert_len;
    *key = s_key;
    *key_len = s_key_len;

    tls_log_fingerprint(s_cert, s_cert_len);
    return ESP_OK;
}

/**
 * @brief 刷新 TLS 证书：若当前 WiFi IP 与证书生成时不同，重新生成证书。
 *
 * 供 WiFi IP 变化（如 STA 重新获取 IP）时调用，使证书 SAN 始终包含
 * 当前 IP，保证浏览器 https://<IP> 访问的名称匹配。
 *
 * @param[out] out_changed 非空时输出本次是否实际重新生成了证书。
 * @return 成功返回 ESP_OK（含未变化未刷新），否则返回错误码。
 */
esp_err_t webserver_tls_refresh(bool *out_changed) {
    if (out_changed != NULL) {
        *out_changed = false;
    }
    if (s_cert == NULL) {
        return ESP_OK; /* 尚未初始化，由 webserver_tls_get 首次处理 */
    }
    char cur[16];
    tls_get_current_ip(cur, sizeof(cur));
    if (strcmp(cur, "0.0.0.0") == 0) {
        return ESP_OK; /* 暂无有效 IP，无需刷新 */
    }
    char old[16] = "";
    if (tls_get_gen_ip(old, sizeof(old)) == ESP_OK && strcmp(old, cur) == 0) {
        return ESP_OK; /* IP 未变化 */
    }

    ESP_LOGI(TAG, "WiFi IP changed (%s), regenerating TLS cert", cur);
    uint8_t *new_cert = NULL, *new_key = NULL;
    size_t new_cert_len = 0, new_key_len = 0;
    esp_err_t err = tls_generate(cur, &new_cert, &new_cert_len, &new_key, &new_key_len);
    if (err != ESP_OK) {
        return err;
    }
    err = tls_save_to_nvs(new_cert, new_cert_len, new_key, new_key_len);
    if (err != ESP_OK) {
        free(new_cert);
        free(new_key);
        return err;
    }
    tls_set_gen_ip(cur);
    free(s_cert);
    free(s_key);
    s_cert = new_cert;
    s_cert_len = new_cert_len;
    s_key = new_key;
    s_key_len = new_key_len;
    if (out_changed != NULL) {
        *out_changed = true;
    }
    tls_log_fingerprint(s_cert, s_cert_len);
    return ESP_OK;
}
