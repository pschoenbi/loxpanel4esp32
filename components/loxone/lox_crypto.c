/*
 * Krypto für die Loxone-Token-Authentifizierung.
 * ESP-IDF 6 bringt mbedTLS 4: die alten Low-Level-APIs (aes, sha, rsa) sind privat,
 * daher alles über PSA. Den RSA-Schlüssel liest mbedtls_pk ein und übergibt ihn an PSA.
 */

#include "lox_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "psa/crypto.h"

static const char *TAG = "lox_crypto";

esp_err_t lox_crypto_init(void)
{
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init: %d", (int)st);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t lox_random(uint8_t *buf, size_t len)
{
    return psa_generate_random(buf, len) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}

void lox_hex_encode(const uint8_t *in, size_t len, char *out, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int lox_hex_decode(const char *hex, uint8_t *out, size_t max)
{
    size_t n = 0;
    while (hex[0] && hex[1] && n < max) {
        int hi = hexval(hex[0]), lo = hexval(hex[1]);
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return (int)n;
}

void lox_url_encode(const char *in, char *out, size_t out_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < out_size; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0f];
        }
    }
    out[o] = '\0';
}

esp_err_t lox_rsa_encrypt_b64(const char *pubkey_b64_der, const uint8_t *in, size_t in_len,
                              char *out, size_t out_size)
{
    uint8_t der[600];
    size_t der_len = 0;
    if (mbedtls_base64_decode(der, sizeof(der), &der_len,
                              (const unsigned char *)pubkey_b64_der, strlen(pubkey_b64_der)) != 0) {
        ESP_LOGE(TAG, "Public Key: Base64 ungültig");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_FAIL;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    psa_key_id_t kid = 0;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

    int rc = mbedtls_pk_parse_public_key(&pk, der, der_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "Public Key nicht lesbar: -0x%04x", -rc);
        goto out;
    }
    rc = mbedtls_pk_get_psa_attributes(&pk, PSA_KEY_USAGE_ENCRYPT, &attr);
    if (rc != 0) {
        ESP_LOGE(TAG, "PSA-Attribute: -0x%04x", -rc);
        goto out;
    }
    psa_set_key_algorithm(&attr, PSA_ALG_RSA_PKCS1V15_CRYPT);
    rc = mbedtls_pk_import_into_psa(&pk, &attr, &kid);
    if (rc != 0) {
        ESP_LOGE(TAG, "Import in PSA: -0x%04x", -rc);
        goto out;
    }

    uint8_t enc[512];
    size_t enc_len = 0;
    psa_status_t st = psa_asymmetric_encrypt(kid, PSA_ALG_RSA_PKCS1V15_CRYPT, in, in_len,
                                             NULL, 0, enc, sizeof(enc), &enc_len);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "RSA-Verschlüsselung: %d", (int)st);
        goto out;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_size, &olen, enc, enc_len) != 0) {
        goto out;
    }
    ret = ESP_OK;

out:
    if (kid) psa_destroy_key(kid);
    psa_reset_key_attributes(&attr);
    mbedtls_pk_free(&pk);
    return ret;
}

esp_err_t lox_aes_encrypt_b64(const uint8_t key[32], const uint8_t iv[16], const char *plain,
                              char *out, size_t out_size)
{
    /* Null-Padding auf volle 16-Byte-Blöcke, mindestens ein Nullbyte */
    size_t len = strlen(plain);
    size_t padded = (len / 16 + 1) * 16;
    uint8_t *buf = calloc(1, padded * 2 + 16);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    memcpy(buf, plain, len);
    uint8_t *enc = buf + padded;

    esp_err_t ret = ESP_FAIL;
    psa_key_id_t kid = 0;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_CBC_NO_PADDING);

    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t n1 = 0, n2 = 0;
    if (psa_import_key(&a, key, 32, &kid) != PSA_SUCCESS ||
        psa_cipher_encrypt_setup(&op, kid, PSA_ALG_CBC_NO_PADDING) != PSA_SUCCESS ||
        psa_cipher_set_iv(&op, iv, 16) != PSA_SUCCESS ||
        psa_cipher_update(&op, buf, padded, enc, padded + 16, &n1) != PSA_SUCCESS ||
        psa_cipher_finish(&op, enc + n1, padded + 16 - n1, &n2) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "AES-Verschlüsselung fehlgeschlagen");
        psa_cipher_abort(&op);
        goto out;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_size, &olen, enc, n1 + n2) == 0) {
        ret = ESP_OK;
    }
out:
    if (kid) psa_destroy_key(kid);
    free(buf);
    return ret;
}

esp_err_t lox_hash_hex(bool sha256, const char *data, char *out, bool upper)
{
    uint8_t h[32];
    size_t hl = 0;
    psa_status_t st = psa_hash_compute(sha256 ? PSA_ALG_SHA_256 : PSA_ALG_SHA_1,
                                       (const uint8_t *)data, strlen(data), h, sizeof(h), &hl);
    if (st != PSA_SUCCESS) return ESP_FAIL;
    lox_hex_encode(h, hl, out, upper);
    return ESP_OK;
}

esp_err_t lox_hmac_hex(bool sha256, const uint8_t *key, size_t key_len, const char *data, char *out)
{
    const psa_algorithm_t alg = PSA_ALG_HMAC(sha256 ? PSA_ALG_SHA_256 : PSA_ALG_SHA_1);
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&a, key_len * 8);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, alg);

    psa_key_id_t kid = 0;
    if (psa_import_key(&a, key, key_len, &kid) != PSA_SUCCESS) return ESP_FAIL;
    uint8_t mac[32];
    size_t ml = 0;
    psa_status_t st = psa_mac_compute(kid, alg, (const uint8_t *)data, strlen(data), mac, sizeof(mac), &ml);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS) return ESP_FAIL;
    lox_hex_encode(mac, ml, out, false);
    return ESP_OK;
}
