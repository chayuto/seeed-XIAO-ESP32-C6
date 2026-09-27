#include "philips_crypto.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto_vectors.h"
#include "esp_log.h"
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"

static const char *TAG = "philips_crypto";
static const char SECRET[] = "JiangPan";
static const char HEX[] = "0123456789ABCDEF";

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = HEX[in[i] >> 4];
        out[2 * i + 1] = HEX[in[i] & 0x0f];
    }
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// counter: 8 hex chars. key and iv: 16 raw bytes each (ASCII hex characters, not decoded).
static void derive(const char *counter, uint8_t key[16], uint8_t iv[16])
{
    uint8_t md5_in[sizeof(SECRET) - 1 + 8], digest[16];
    char hex[32];
    memcpy(md5_in, SECRET, sizeof(SECRET) - 1);
    memcpy(md5_in + sizeof(SECRET) - 1, counter, 8);
    mbedtls_md5(md5_in, sizeof(md5_in), digest);
    to_hex(digest, 16, hex);
    memcpy(key, hex, 16);
    memcpy(iv, hex + 16, 16);
}

static void sha256_hex(const char *a, size_t alen, const char *b, size_t blen, char out[64])
{
    mbedtls_sha256_context ctx;
    uint8_t d[32];
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)a, alen);
    mbedtls_sha256_update(&ctx, (const uint8_t *)b, blen);
    mbedtls_sha256_finish(&ctx, d);
    mbedtls_sha256_free(&ctx);
    to_hex(d, 32, out);
}

esp_err_t philips_decrypt(const char *in, size_t in_len, char *out, size_t out_cap, size_t *out_len)
{
    // counter (8) + at least one AES block (32 hex) + digest (64)
    if (in_len < 8 + 32 + 64) return ESP_ERR_INVALID_SIZE;
    size_t ct_hex_len = in_len - 8 - 64;
    if (ct_hex_len % 32 != 0) return ESP_ERR_INVALID_SIZE;
    size_t ct_len = ct_hex_len / 2;

    // Integrity first: a tampered or foreign payload is reported as such, whatever its size.
    char digest[64];
    sha256_hex(in, 8, in + 8, ct_hex_len, digest);
    if (memcmp(digest, in + 8 + ct_hex_len, 64) != 0) return ESP_ERR_INVALID_CRC;
    if (ct_len + 1 > out_cap) return ESP_ERR_INVALID_SIZE;

    uint8_t *ct = malloc(ct_len);
    if (!ct) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < ct_len; i++) {
        int hi = nibble(in[8 + 2 * i]), lo = nibble(in[8 + 2 * i + 1]);
        if (hi < 0 || lo < 0) {
            free(ct);
            return ESP_ERR_INVALID_ARG;
        }
        ct[i] = (uint8_t)(hi << 4 | lo);
    }

    uint8_t key[16], iv[16];
    derive(in, key, iv);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, key, 128);
    int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, ct_len, iv, ct, (uint8_t *)out);
    mbedtls_aes_free(&aes);
    free(ct);
    if (rc != 0) return ESP_FAIL;

    uint8_t pad = (uint8_t)out[ct_len - 1];
    if (pad == 0 || pad > 16) return ESP_ERR_INVALID_ARG;
    for (size_t i = ct_len - pad; i < ct_len; i++) {
        if ((uint8_t)out[i] != pad) return ESP_ERR_INVALID_ARG;
    }
    out[ct_len - pad] = '\0';
    if (out_len) *out_len = ct_len - pad;
    return ESP_OK;
}

esp_err_t philips_encrypt(uint32_t *counter, const char *plaintext, char *out, size_t out_cap)
{
    size_t pt_len = strlen(plaintext);
    size_t padded = (pt_len / 16 + 1) * 16; // PKCS#7 always adds 1..16 bytes
    size_t need = 8 + padded * 2 + 64 + 1;
    if (need > out_cap) return ESP_ERR_INVALID_SIZE;

    *counter += 1; // wraps at 2^32 by unsigned arithmetic, as the reference does
    char ctr[9];
    snprintf(ctr, sizeof(ctr), "%08" PRIX32, *counter);

    uint8_t *buf = malloc(padded * 2); // plaintext then ciphertext
    if (!buf) return ESP_ERR_NO_MEM;
    uint8_t *pt = buf, *ct = buf + padded;
    memcpy(pt, plaintext, pt_len);
    memset(pt + pt_len, (int)(padded - pt_len), padded - pt_len);

    uint8_t key[16], iv[16];
    derive(ctr, key, iv);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, iv, pt, ct);
    mbedtls_aes_free(&aes);
    if (rc != 0) {
        free(buf);
        return ESP_FAIL;
    }
    memcpy(out, ctr, 8);
    to_hex(ct, padded, out + 8);
    free(buf);
    sha256_hex(out, 8, out + 8, padded * 2, out + 8 + padded * 2);
    out[8 + padded * 2 + 64] = '\0';
    return ESP_OK;
}

esp_err_t philips_crypto_selftest(void)
{
    int failed = 0;
    for (size_t i = 0; i < sizeof(CRYPTO_VECTORS) / sizeof(CRYPTO_VECTORS[0]); i++) {
        const crypto_vector_t *v = &CRYPTO_VECTORS[i];
        size_t enc_len = strlen(v->encrypted), pt_len = strlen(v->plaintext);
        char *buf = malloc(enc_len + 1);
        if (!buf) return ESP_ERR_NO_MEM;

        uint32_t ctr = (uint32_t)strtoul(v->counter_before, NULL, 16);
        bool enc_ok = philips_encrypt(&ctr, v->plaintext, buf, enc_len + 1) == ESP_OK &&
                      strcmp(buf, v->encrypted) == 0;
        size_t dec_len = 0;
        bool dec_ok = philips_decrypt(v->encrypted, enc_len, buf, enc_len + 1, &dec_len) == ESP_OK &&
                      dec_len == pt_len && memcmp(buf, v->plaintext, pt_len) == 0;
        // Flip one ciphertext character: the digest check must reject it.
        memcpy(buf, v->encrypted, enc_len);
        buf[10] = buf[10] == 'A' ? 'B' : 'A';
        char out[32];
        bool tamper_ok = philips_decrypt(buf, enc_len, out, sizeof(out), NULL) == ESP_ERR_INVALID_CRC;
        free(buf);

        bool ok = enc_ok && dec_ok && tamper_ok;
        failed += !ok;
        ESP_LOGI(TAG, "selftest vector=%s bytes=%u encrypt=%s decrypt=%s tamper=%s", v->name,
                 (unsigned)pt_len, enc_ok ? "ok" : "FAIL", dec_ok ? "ok" : "FAIL",
                 tamper_ok ? "rejected" : "FAIL");
    }
    return failed ? ESP_FAIL : ESP_OK;
}
