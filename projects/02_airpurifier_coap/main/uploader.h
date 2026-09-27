#pragma once
// Background upload of purifier_reading and purifier_device_status rows to Supabase.
// Rows wait in a RAM ring until they're stored; one row per POST, so a duplicate (409,
// already stored) can never take a new row down with it the way a batch would.
#include <stdbool.h>
#include <stdint.h>
#include "bucket.h"

typedef struct {
    int64_t ts_ms; // floored to the 5-minute mark, so a retry mints the same id
    char purifier_id[40], purifier_name[32], purifier_model[24], purifier_fw[16];
    uint32_t free_heap, min_heap, uptime_s, wifi_drops, observes, syncs, updates;
    int wifi_rssi;
    int32_t last_update_age_s; // -1 = no status since boot
} status_row_t;

typedef struct {
    uint32_t sent, duplicates, failures, dropped, depth, depth_max;
    int32_t last_ok_age_s; // -1 = nothing stored yet
} uploader_stats_t;

// ~6 h of readings (120 × 3 min) plus the 5-minute status rows for the same span.
#define UPLOADER_RING 200

void uploader_start(const char *device_id);
void uploader_push_reading(const reading_row_t *r);
void uploader_push_status(const status_row_t *s);
void uploader_get_stats(uploader_stats_t *out);

// Test hook: for `seconds`, POST to a table that doesn't exist, so uploads fail through
// the real HTTP path (404) and rows pile up in the ring. 0 ends it early.
void uploader_simulate_outage(uint32_t seconds);
