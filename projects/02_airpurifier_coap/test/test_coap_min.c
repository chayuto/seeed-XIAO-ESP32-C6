// Host test for coap_min.c:  cc -I../main -o /tmp/t test_coap_min.c ../main/coap_min.c && /tmp/t
#include <stdio.h>
#include <string.h>
#include "coap_min.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    uint8_t buf[256];
    const uint8_t tok[2] = {0x66, 0x96};

    // The GET aiocoap sends, byte for byte (captured 2026-09-27).
    size_t n = coap_build(buf, sizeof buf, COAP_TYPE_NON, COAP_GET, 2, tok, 2, "/sys/dev/status", 0, NULL, 0);
    const uint8_t want_get[] = {0x52,0x01,0x00,0x02,0x66,0x96,0x60,0x53,'s','y','s',0x03,'d','e','v',0x06,'s','t','a','t','u','s'};
    CHECK(n == sizeof want_get && memcmp(buf, want_get, n) == 0);

    // Sync POST with payload.
    n = coap_build(buf, sizeof buf, COAP_TYPE_NON, COAP_POST, 0x1234, NULL, 0, "/sys/dev/sync", COAP_NO_OBSERVE,
                   (const uint8_t *)"0A1B2C3D", 8);
    const uint8_t want_sync[] = {0x50,0x02,0x12,0x34,0xb3,'s','y','s',0x03,'d','e','v',0x04,'s','y','n','c',0xff,'0','A','1','B','2','C','3','D'};
    CHECK(n == sizeof want_sync && memcmp(buf, want_sync, n) == 0);

    // Response whose MID and token contain 0xFF: the naive split broke on this.
    const uint8_t resp[] = {0x52,0x45,0x00,0xff, 0xff,0x10, 0x61,0x05, 0x61,0x00, 0x21,0x3c, 0xff,'A','B'};
    coap_msg_t m;
    CHECK(coap_parse(resp, sizeof resp, &m));
    CHECK(m.type == COAP_TYPE_NON && m.code == 0x45 && m.mid == 0x00ff);
    CHECK(m.tkl == 2 && m.token[0] == 0xff && m.token[1] == 0x10);
    CHECK(m.has_observe && m.observe == 5);
    CHECK(m.has_max_age && m.max_age == 60);
    CHECK(m.payload_len == 2 && memcmp(m.payload, "AB", 2) == 0);

    // Round trip, plus extended option length (a 20-char segment uses the 13 form).
    n = coap_build(buf, sizeof buf, COAP_TYPE_CON, COAP_GET, 7, tok, 2, "/abcdefghijklmnopqrst", 300, (const uint8_t *)"x", 1);
    CHECK(n > 0 && coap_parse(buf, n, &m));
    CHECK(m.has_observe && m.observe == 300 && m.payload_len == 1 && m.payload[0] == 'x');

    // Malformed: truncated option, marker without payload, bad version, too-small buffer.
    const uint8_t trunc[] = {0x50,0x45,0,1, 0x35,'a'};
    CHECK(!coap_parse(trunc, sizeof trunc, &m));
    const uint8_t marker_only[] = {0x50,0x45,0,1, 0xff};
    CHECK(!coap_parse(marker_only, sizeof marker_only, &m));
    const uint8_t badver[] = {0x90,0x45,0,1};
    CHECK(!coap_parse(badver, sizeof badver, &m));
    CHECK(coap_build(buf, 10, COAP_TYPE_NON, COAP_GET, 1, tok, 2, "/sys/dev/status", 0, NULL, 0) == 0);

    printf("%s (%d failures)\n", fails ? "FAILED" : "coap_min ok", fails);
    return fails != 0;
}
