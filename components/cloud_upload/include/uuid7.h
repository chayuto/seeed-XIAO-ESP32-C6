#pragma once
// Deterministic UUIDv7 (RFC 9562 layout), ported from the Govee monitor
// (ws-ESP32-C6-Touch-AMOLED-1.8/projects/18_govee_monitor/main/uuid7.c).
//
// The id is derived from (epoch_ms, device_id, source) instead of randomness, so a
// re-sent row gets a byte-identical id: the server rejects it as a duplicate (409 /
// 23505) and retrying is always safe. rand_a/rand_b are an FNV-1a hash of the inputs,
// a deliberate deviation from RFC 9562; layout, version and variant bits conform, and the
// 48-bit ms prefix still sorts by time.
//
// Generalised from Govee's 6-byte MAC to any source identifier (the purifier's DeviceId
// string here). The hash input layout below is frozen: changing it changes every id.
#include <stddef.h>
#include <stdint.h>

#define UUID7_STR_LEN 37 // 36 chars + NUL

// Writes a lowercase hyphenated UUIDv7 into out[UUID7_STR_LEN].
void uuid7_deterministic(int64_t epoch_ms, const char *device_id, const void *source,
                         size_t source_len, char *out);

// Milliseconds encoded in the leading 48 bits, or -1. For tests and diagnostics.
int64_t uuid7_extract_ms(const char *uuid);
