#pragma once
// Philips air purifier CoAP payload crypto, ported from aioairctrl 0.3.1 coap/encryption.py.
//
// Wire format (uppercase ASCII hex): [counter:8][AES-128-CBC ciphertext][SHA256(counter+ct):64]
// key||iv = MD5("JiangPan" + counter) as 32 uppercase hex chars; the first 16 characters,
// used as raw ASCII bytes, are the AES key and the last 16 the IV. PKCS#7 padding.
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Decrypt `in` (length `in_len`, not necessarily NUL-terminated) into `out` as a
// NUL-terminated string. ESP_ERR_INVALID_CRC = digest mismatch (tampered, truncated or not
// this protocol); ESP_ERR_INVALID_SIZE = bad length / `out` too small;
// ESP_ERR_INVALID_ARG = not hex or bad padding.
esp_err_t philips_decrypt(const char *in, size_t in_len, char *out, size_t out_cap, size_t *out_len);

// Increment *counter (wrapping at 2^32), then encrypt `plaintext` with it into `out`
// (NUL-terminated). Needed for /sys/dev/control; used by the self-test now.
esp_err_t philips_encrypt(uint32_t *counter, const char *plaintext, char *out, size_t out_cap);

// Check both directions against the vectors generated from aioairctrl. Logs one line per
// vector and returns ESP_OK only if all pass.
esp_err_t philips_crypto_selftest(void);
