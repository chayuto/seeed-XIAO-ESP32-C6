#pragma once
// Minimal CoAP (RFC 7252) message codec: just what the Philips purifier protocol needs.
// No sockets and no ESP-IDF dependencies, so it also builds on the host for tests.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COAP_TYPE_CON 0
#define COAP_TYPE_NON 1
#define COAP_TYPE_ACK 2
#define COAP_TYPE_RST 3

#define COAP_GET  0x01
#define COAP_POST 0x02
#define COAP_CODE_CLASS(c) ((c) >> 5)          // 2 = success, 4 = client error, ...
#define COAP_CODE_DETAIL(c) ((c) & 0x1f)

#define COAP_OPT_OBSERVE 6
#define COAP_OPT_URI_PATH 11
#define COAP_OPT_CONTENT_FORMAT 12
#define COAP_OPT_MAX_AGE 14

#define COAP_NO_OBSERVE (-1)

typedef struct {
    uint8_t type;
    uint8_t code;
    uint16_t mid;
    uint8_t tkl;
    uint8_t token[8];
    bool has_observe;
    uint32_t observe;
    bool has_max_age;
    uint32_t max_age;
    const uint8_t *payload; // points into the parsed buffer; NULL when there is none
    size_t payload_len;
} coap_msg_t;

// Build a request. `path` like "/sys/dev/status" becomes one Uri-Path option per segment.
// `observe` is COAP_NO_OBSERVE or the Observe value (0 = register). Returns the encoded
// length, or 0 if it does not fit in `cap` or an argument is invalid.
size_t coap_build(uint8_t *buf, size_t cap, uint8_t type, uint8_t code, uint16_t mid,
                  const uint8_t *token, uint8_t tkl, const char *path, int32_t observe,
                  const uint8_t *payload, size_t payload_len);

// Parse a datagram. Walks every option to find the payload marker, so a 0xFF byte in the
// message ID, token or an option value is not mistaken for it. Returns false if malformed.
bool coap_parse(const uint8_t *buf, size_t len, coap_msg_t *out);
