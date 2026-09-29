#pragma once
// CSI capture and the per-second motion score. Frames come from pinging the gateway; only
// frames sent by the associated AP are used (see docs/design/03_csi_presence.md).
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    float motion;     // mean coefficient of variation across subcarriers, x1000
    uint32_t n;       // frames in the window
    int rssi;         // mean RSSI of those frames
    int nf;           // noise floor of the last frame
} csi_second_t;

typedef struct {
    uint32_t frames;   // AP frames used
    uint32_t other;    // CSI frames from other senders, dropped
    uint32_t dropped;  // queue full
    uint32_t skip_len; // AP frames whose length didn't match the lock
    uint32_t trunc;    // longer than our buffer
    uint32_t relocks;
    uint32_t raw_lines; // raw dump lines handed to the console
    uint32_t raw_drop;  // raw dump lines dropped because the console fell behind
    uint16_t lock_len;
    int64_t last_frame_us; // 0 = never
} csi_stats_t;

typedef void (*csi_second_cb_t)(const csi_second_t *s);

// Call once Wi-Fi has an IP. Starts CSI, the processing task and the gateway ping.
// on_second runs in the CSI task once per second.
esp_err_t csi_start(csi_second_cb_t on_second);

void csi_get_stats(csi_stats_t *out);
void csi_set_raw_dump(bool on);
bool csi_raw_dump(void);
