#pragma once
// Turns purifier statuses into 3-minute purifier_reading rows (see the design doc,
// "Cloud logging"). Buckets are keyed by their END time in epoch ms, aligned to multiples
// of 3 minutes, so a window always gets the same id. Only CLOSED buckets are emitted: an
// id is final once minted, so uploading an open window would turn its complete upload
// into a 409 and lose the stats.
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#define BUCKET_MS (3 * 60 * 1000)

typedef struct {
    bool valid; // a status with a DeviceId has been seen since boot
    char purifier_id[40], name[32], model[24], fw[16];
    int pwr, mode, fan, pm25, iai, err, prefilter_h, hepa_h, dev_rssi;
} purifier_state_t;

typedef struct {
    int64_t end_ms;
    int n_samples;             // 0 = carried forward from the last-known state
    float pm25_mean;           // < 0 = unknown
    int pm25_min, pm25_max, iai_max;
    int pwr, mode, fan, err, prefilter_h, hepa_h, dev_rssi; // -1 = unknown
    char purifier_id[40];
} reading_row_t;

// Parse state.reported into the last-known state and, once the clock is synced, add a
// sample to the bucket it falls in. `epoch_ms` is -1 before SNTP sync.
void bucket_on_status(const cJSON *reported, int64_t epoch_ms);

const purifier_state_t *bucket_state(void);

// End of the bucket containing epoch_ms.
int64_t bucket_end_for(int64_t epoch_ms);

// Produce the row for the (closed) window ending at end_ms. With samples in it: their
// stats. Without: the last-known values with n_samples = 0, but only if carry_forward is
// true (the caller has just confirmed the purifier answers sync). Returns false when there's
// nothing to emit (no samples and no carry-forward, or no state yet).
bool bucket_close(int64_t end_ms, bool carry_forward, reading_row_t *out);

// Samples collected so far for the window ending at end_ms (0 if none).
int bucket_samples(int64_t end_ms);
