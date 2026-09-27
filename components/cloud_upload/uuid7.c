#include "uuid7.h"
#include <string.h>

#define FNV_BASIS 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL
#define SEED_B 0x9E3779B97F4A7C15ULL // golden ratio, decorrelates the second hash

static uint64_t fnv1a64(const uint8_t *p, size_t len, uint64_t h)
{
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

void uuid7_deterministic(int64_t epoch_ms, const char *device_id, const void *source,
                         size_t source_len, char *out)
{
    // Frozen hash input: device_id (<=40 B), 0x00, source (<=64 B), 0x00, epoch_ms (8 B BE).
    // The separators stop ("ab","c") and ("a","bc") hashing alike.
    uint8_t buf[40 + 1 + 64 + 1 + 8];
    size_t n = 0;
    size_t dlen = device_id ? strlen(device_id) : 0;
    if (dlen > 40) dlen = 40;
    memcpy(buf + n, device_id, dlen);
    n += dlen;
    buf[n++] = 0;
    if (source_len > 64) source_len = 64;
    if (source_len) memcpy(buf + n, source, source_len);
    n += source_len;
    buf[n++] = 0;
    for (int i = 7; i >= 0; i--) buf[n++] = (uint8_t)((uint64_t)epoch_ms >> (i * 8));

    uint64_t h1 = fnv1a64(buf, n, FNV_BASIS);
    uint64_t h2 = fnv1a64(buf, n, FNV_BASIS ^ SEED_B);

    uint8_t b[16];
    for (int i = 0; i < 6; i++) b[i] = (uint8_t)((uint64_t)epoch_ms >> ((5 - i) * 8));
    b[6] = (uint8_t)(0x70 | ((h1 >> 8) & 0x0F)); // version 7 + rand_a hi
    b[7] = (uint8_t)(h1 & 0xFF);
    b[8] = (uint8_t)(0x80 | ((h2 >> 56) & 0x3F)); // variant 0b10 + rand_b
    for (int i = 9; i < 16; i++) b[i] = (uint8_t)(h2 >> ((15 - i) * 8));

    static const char hex[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hex[b[i] >> 4];
        out[o++] = hex[b[i] & 0x0F];
    }
    out[o] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int64_t uuid7_extract_ms(const char *uuid)
{
    int64_t ms = 0;
    int got = 0;
    for (const char *p = uuid; *p && got < 12; p++) {
        int v = hexval(*p);
        if (v < 0) continue; // hyphens
        ms = (ms << 4) | v;
        got++;
    }
    return got == 12 ? ms : -1;
}
