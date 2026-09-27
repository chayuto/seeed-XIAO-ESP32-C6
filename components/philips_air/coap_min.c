#include "coap_min.h"
#include <string.h>

// Option delta/length nibble with RFC 7252 extended forms (13: +1 byte, 14: +2 bytes).
static size_t put_ext(uint8_t *nib, uint8_t *ext, uint32_t v, int shift)
{
    if (v < 13) {
        *nib |= (uint8_t)(v << shift);
        return 0;
    }
    if (v < 269) {
        *nib |= (uint8_t)(13 << shift);
        ext[0] = (uint8_t)(v - 13);
        return 1;
    }
    *nib |= (uint8_t)(14 << shift);
    ext[0] = (uint8_t)((v - 269) >> 8);
    ext[1] = (uint8_t)(v - 269);
    return 2;
}

static size_t put_option(uint8_t *buf, size_t cap, size_t pos, uint16_t *last,
                         uint16_t number, const uint8_t *val, size_t len)
{
    uint8_t hdr = 0, ext[4];
    size_t n = put_ext(&hdr, ext, number - *last, 4);
    n += put_ext(&hdr, ext + n, (uint32_t)len, 0);
    if (pos + 1 + n + len > cap) return 0;
    buf[pos] = hdr;
    memcpy(buf + pos + 1, ext, n);
    memcpy(buf + pos + 1 + n, val, len);
    *last = number;
    return 1 + n + len;
}

size_t coap_build(uint8_t *buf, size_t cap, uint8_t type, uint8_t code, uint16_t mid,
                  const uint8_t *token, uint8_t tkl, const char *path, int32_t observe,
                  const uint8_t *payload, size_t payload_len)
{
    if (tkl > 8 || cap < 4u + tkl || type > 3) return 0;
    buf[0] = (uint8_t)(0x40 | (type << 4) | tkl);
    buf[1] = code;
    buf[2] = (uint8_t)(mid >> 8);
    buf[3] = (uint8_t)mid;
    memcpy(buf + 4, token, tkl);
    size_t pos = 4 + tkl, n;
    uint16_t last = 0;

    // Options must go out in ascending number order: Observe (6) before Uri-Path (11).
    if (observe != COAP_NO_OBSERVE) {
        uint8_t v[3];
        size_t vl = 0;
        uint32_t o = (uint32_t)observe; // minimal big-endian; 0 is the empty value
        if (o > 0xffff) v[vl++] = (uint8_t)(o >> 16);
        if (o > 0xff) v[vl++] = (uint8_t)(o >> 8);
        if (o > 0) v[vl++] = (uint8_t)o;
        if (!(n = put_option(buf, cap, pos, &last, COAP_OPT_OBSERVE, v, vl))) return 0;
        pos += n;
    }
    for (const char *seg = path; seg && *seg;) {
        while (*seg == '/') seg++;
        if (!*seg) break;
        const char *end = strchr(seg, '/');
        size_t sl = end ? (size_t)(end - seg) : strlen(seg);
        if (!(n = put_option(buf, cap, pos, &last, COAP_OPT_URI_PATH, (const uint8_t *)seg, sl))) return 0;
        pos += n;
        seg += sl;
    }
    if (payload_len) {
        if (pos + 1 + payload_len > cap) return 0;
        buf[pos++] = 0xff;
        memcpy(buf + pos, payload, payload_len);
        pos += payload_len;
    }
    return pos;
}

static bool get_ext(const uint8_t **p, const uint8_t *end, uint32_t nib, uint32_t *out)
{
    if (nib < 13) {
        *out = nib;
    } else if (nib == 13) {
        if (*p + 1 > end) return false;
        *out = 13u + (*p)[0];
        *p += 1;
    } else if (nib == 14) {
        if (*p + 2 > end) return false;
        *out = 269u + (((uint32_t)(*p)[0] << 8) | (*p)[1]);
        *p += 2;
    } else {
        return false; // 15 is reserved except as the payload marker
    }
    return true;
}

static uint32_t uint_value(const uint8_t *v, uint32_t len)
{
    uint32_t x = 0;
    for (uint32_t i = 0; i < len && i < 4; i++) x = (x << 8) | v[i];
    return x;
}

bool coap_parse(const uint8_t *buf, size_t len, coap_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 4 || (buf[0] >> 6) != 1) return false;
    out->type = (buf[0] >> 4) & 3;
    out->tkl = buf[0] & 0x0f;
    out->code = buf[1];
    out->mid = (uint16_t)((buf[2] << 8) | buf[3]);
    if (out->tkl > 8 || len < 4u + out->tkl) return false;
    memcpy(out->token, buf + 4, out->tkl);

    const uint8_t *p = buf + 4 + out->tkl, *end = buf + len;
    uint32_t number = 0;
    while (p < end) {
        if (*p == 0xff) {
            p++;
            if (p == end) return false; // marker with no payload is a format error
            out->payload = p;
            out->payload_len = (size_t)(end - p);
            return true;
        }
        uint32_t delta, olen;
        uint8_t hdr = *p++;
        if (!get_ext(&p, end, hdr >> 4, &delta) || !get_ext(&p, end, hdr & 0x0f, &olen)) return false;
        if (p + olen > end) return false;
        number += delta;
        if (number == COAP_OPT_OBSERVE) {
            out->has_observe = true;
            out->observe = uint_value(p, olen);
        } else if (number == COAP_OPT_MAX_AGE) {
            out->has_max_age = true;
            out->max_age = uint_value(p, olen);
        }
        p += olen;
    }
    return true;
}
