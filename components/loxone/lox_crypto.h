#pragma once

/* Krypto-Helfer für die Loxone-Authentifizierung (PSA / mbedTLS 4). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t lox_crypto_init(void);
esp_err_t lox_random(uint8_t *buf, size_t len);

/* Öffentlicher Schlüssel des Miniservers (Base64-DER) -> RSA-PKCS#1-v1.5-verschlüsselt, Base64. */
esp_err_t lox_rsa_encrypt_b64(const char *pubkey_b64_der, const uint8_t *in, size_t in_len,
                              char *out, size_t out_size);

/* AES-256-CBC mit Null-Padding, Ergebnis Base64. */
esp_err_t lox_aes_encrypt_b64(const uint8_t key[32], const uint8_t iv[16], const char *plain,
                              char *out, size_t out_size);

/* SHA1/SHA256 als Hex-String (out >= 65 Bytes). */
esp_err_t lox_hash_hex(bool sha256, const char *data, char *out, bool upper);

/* HMAC-SHA1/SHA256 als Hex-String (out >= 65 Bytes). */
esp_err_t lox_hmac_hex(bool sha256, const uint8_t *key, size_t key_len, const char *data, char *out);

void lox_hex_encode(const uint8_t *in, size_t len, char *out, bool upper);
int lox_hex_decode(const char *hex, uint8_t *out, size_t max);

/* URI-Kodierung (alles ausser A-Z a-z 0-9 - _ . ~). */
void lox_url_encode(const char *in, char *out, size_t out_size);
