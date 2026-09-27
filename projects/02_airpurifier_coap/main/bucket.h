#pragma once
// Turns purifier statuses into 3-minute purifier_reading rows (see the design doc,
// "Cloud logging"). Buckets are keyed by their END time in epoch ms, aligned to
// multiples of 3 minutes, so the same window always gets the same id.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define BUCKET_MS (3 * 60 * 1000)

typedef struct {
    bool valid; // a status has been seen since boot
    char purifier_id[40], name[32], model[24], fw[16];
    int pwr, mode, fan, pm25, iai, err, prefilter_h, hepa_h, dev_rssi;
} purifier_state_t;

// Parse state.reported into the last-known state and, once the clock is synced, add a
// sample to the bucket it falls in. `epoch_ms` is -1 before SNTP sync.
void bucket_on_status(const cJSON *reported, int64_t epoch_ms);

const purifier_state_t *bucket_state(void);

// End of the bucket containing epoch_ms.
int64_t bucket_end_for(int64_t epoch_ms);

// Build a one-row JSON array for the bucket ending at end_ms. With samples in that bucket
// it carries their stats; without, the last-known values with n_samples = 0 (the caller
// decides whether carrying forward is justified). Returns false if there's no state yet.
bool bucket_row_json(int64_t end_ms, const char *device_id, char *out, size_t cap, int *n_samples);
