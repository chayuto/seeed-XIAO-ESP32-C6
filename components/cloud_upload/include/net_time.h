#pragma once
// Wall-clock time from SNTP for a board with no RTC (the XIAO). Idea from the Govee
// monitor's net_time.c: keep one epoch-minus-uptime offset, so anything stamped with
// esp_timer uptime (even before the first sync) resolves to the right instant later.
#include <stdbool.h>
#include <stdint.h>

// Start SNTP (call once, after Wi-Fi has an IP). Non-blocking.
void net_time_start(void);

bool net_time_synced(void);

// Wall clock in ms since the epoch for a given esp_timer uptime (us), or -1 before sync.
int64_t net_time_epoch_ms_at(int64_t uptime_us);
int64_t net_time_epoch_ms_now(void);

// Seconds since the last successful sync, or -1 if never.
int64_t net_time_since_sync_s(void);
