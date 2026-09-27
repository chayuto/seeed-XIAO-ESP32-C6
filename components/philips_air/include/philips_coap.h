#pragma once
// Client for the Philips air purifier's local CoAP API (sync + status), over one UDP socket.
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int sock;
    uint16_t next_mid;
    uint8_t token[4];         // one token per session: Observe notifications reuse it
    char client_key[9];       // from /sys/dev/sync; the counter for outgoing encryption
    uint32_t max_age;         // from the last status reply (default 60)
    uint32_t last_observe;    // Observe sequence number of the last status reply
    uint32_t last_latency_ms; // sync request -> reply
    int64_t observe_sent_us;  // when the last Observe GET went out
    uint32_t since_observe_ms;// arrival of the last status, relative to observe_sent_us
    uint32_t send_us;         // how long the last send() took
    uint32_t stray;           // datagrams ignored (wrong token, not CoAP, ...)
} philips_t;

esp_err_t philips_open(philips_t *p, const char *host, uint16_t port);

// Find a purifier on the local network by sending /sys/dev/sync to the subnet broadcast
// address (3 tries), falling back to a batched unicast sweep of the /24, and taking the
// first valid reply (8 hex chars). Purifiers ignore ping and their MAC prefixes aren't
// predictable, so sync is the only reliable probe. Worst case ~15 s (feed watchdogs around
// it). Writes the dotted IP into host_out.
esp_err_t philips_discover(uint16_t port, uint32_t timeout_ms, char *host_out, size_t host_cap);
void philips_close(philips_t *p);

// POST /sys/dev/sync with a random nonce; stores the device's client key.
esp_err_t philips_sync(philips_t *p);

// Register (or re-register) an Observe on /sys/dev/status: sends the GET and returns
// without waiting. The first status and every later change arrive via philips_recv_status.
esp_err_t philips_observe(philips_t *p);

// Wait up to timeout_ms for the next status datagram carrying our token, verify + decrypt
// it into `json`. ESP_ERR_TIMEOUT when nothing arrived. Sets p->last_observe/max_age and
// p->since_observe_ms (time since the last philips_observe call).
esp_err_t philips_recv_status(philips_t *p, char *json, size_t json_cap, uint32_t timeout_ms);
