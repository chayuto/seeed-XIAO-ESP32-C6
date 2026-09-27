#include "philips_coap.h"
#include <string.h>
#include <sys/time.h>
#include "coap_min.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "philips_crypto.h"
#include "sdkconfig.h"

static const char *TAG = "philips_coap";

// The AC2220's status reply is ~2.1 KB (2088 hex chars of payload) in one datagram.
#define RX_CAP 4096

static uint8_t s_rx[RX_CAP];

esp_err_t philips_open(philips_t *p, const char *host, uint16_t port)
{
    memset(p, 0, sizeof(*p));
    p->sock = -1;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "bad host '%s'", host);
        return ESP_ERR_INVALID_ARG;
    }
    p->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (p->sock < 0) return ESP_FAIL;
    // connect() on UDP: send() goes to the purifier and recv() only sees its datagrams.
    if (connect(p->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "connect errno=%d", errno);
        philips_close(p);
        return ESP_FAIL;
    }
    p->next_mid = (uint16_t)esp_random();
    uint32_t t = esp_random();
    memcpy(p->token, &t, sizeof(p->token));
    p->max_age = 60;
    ESP_LOGI(TAG, "open host=%s port=%u", host, port);
    return ESP_OK;
}

void philips_close(philips_t *p)
{
    if (p->sock >= 0) close(p->sock);
    p->sock = -1;
}

static esp_err_t send_req(philips_t *p, uint8_t code, const char *path, int32_t observe,
                         const uint8_t *payload, size_t plen)
{
    uint8_t tx[128];
    uint16_t mid = p->next_mid++;
    size_t n = coap_build(tx, sizeof(tx), COAP_TYPE_NON, code, mid, p->token, sizeof(p->token),
                          path, observe, payload, plen);
    if (!n) return ESP_ERR_INVALID_SIZE;
    int64_t t = esp_timer_get_time();
    ssize_t sent = send(p->sock, tx, n, 0);
    p->send_us = (uint32_t)(esp_timer_get_time() - t);
    if (sent != (ssize_t)n) {
        ESP_LOGW(TAG, "send path=%s errno=%d", path, errno);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "sent path=%s mid=%u len=%u send_us=%lu", path, mid, (unsigned)n, (unsigned long)p->send_us);
    return ESP_OK;
}

// Wait for the next datagram carrying our token. Others (not CoAP, foreign token) are
// counted and skipped.
static esp_err_t wait_reply(philips_t *p, const char *path, uint32_t timeout_ms, coap_msg_t *reply)
{
    int64_t t0 = esp_timer_get_time();
    int64_t deadline = t0 + (int64_t)timeout_ms * 1000;
    for (;;) {
        // SO_RCVTIMEO is whole milliseconds and 0 means "block forever" in lwIP. The first
        // version passed the sub-millisecond remainder here, so a recv after the deadline
        // nearly expired blocked until the next datagram (16.7 s against a 12 s timeout,
        // asked_ms=0 in the log, 2026-09-27). Round up, and stop below 1 ms.
        int64_t left_ms = (deadline - esp_timer_get_time() + 999) / 1000;
        if (left_ms < 1) return ESP_ERR_TIMEOUT;
        struct timeval tv = {.tv_sec = left_ms / 1000, .tv_usec = (left_ms % 1000) * 1000};
        setsockopt(p->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int64_t w = esp_timer_get_time();
        int r = recv(p->sock, s_rx, sizeof(s_rx), 0);
        int64_t blocked_ms = (esp_timer_get_time() - w) / 1000;
        if (blocked_ms > left_ms + 1000) {
            ESP_LOGW(TAG, "recv overran asked_ms=%lld blocked_ms=%lld r=%d", (long long)left_ms,
                     (long long)blocked_ms, r);
        }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // loop re-checks deadline
            ESP_LOGW(TAG, "recv errno=%d", errno);
            return ESP_FAIL;
        }
        if (r == sizeof(s_rx)) ESP_LOGW(TAG, "datagram filled rx buffer (%d B): may be truncated", r);
        if (!coap_parse(s_rx, (size_t)r, reply)) {
            p->stray++;
            ESP_LOGW(TAG, "unparseable datagram len=%d", r);
            continue;
        }
        if (reply->tkl != sizeof(p->token) || memcmp(reply->token, p->token, sizeof(p->token)) != 0) {
            p->stray++;
            ESP_LOGD(TAG, "stray datagram mid=%u tkl=%u len=%d", reply->mid, reply->tkl, r);
            continue;
        }
        // For sync this is the request->reply latency. For status it is only how long this
        // wait lasted; since_observe_ms is the meaningful number there.
        p->last_latency_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        ESP_LOGD(TAG, "rx path=%s code=%d.%02d mid=%u len=%d", path, COAP_CODE_CLASS(reply->code),
                 COAP_CODE_DETAIL(reply->code), reply->mid, r);
        if (COAP_CODE_CLASS(reply->code) != 2) {
            ESP_LOGW(TAG, "error reply path=%s code=%d.%02d", path, COAP_CODE_CLASS(reply->code),
                     COAP_CODE_DETAIL(reply->code));
            return ESP_FAIL;
        }
        return ESP_OK;
    }
}

esp_err_t philips_sync(philips_t *p)
{
    char nonce[9];
    snprintf(nonce, sizeof(nonce), "%08lX", (unsigned long)esp_random());
    coap_msg_t reply;
    esp_err_t err = send_req(p, COAP_POST, "/sys/dev/sync", COAP_NO_OBSERVE, (const uint8_t *)nonce, 8);
    if (err == ESP_OK) err = wait_reply(p, "/sys/dev/sync", CONFIG_PURIFIER_TIMEOUT_MS, &reply);
    if (err == ESP_ERR_TIMEOUT) ESP_LOGW(TAG, "sync timeout after=%dms", CONFIG_PURIFIER_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    if (reply.payload_len != 8) {
        ESP_LOGW(TAG, "sync reply payload_len=%u, want 8", (unsigned)reply.payload_len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(p->client_key, reply.payload, 8);
    p->client_key[8] = '\0';
    ESP_LOGI(TAG, "sync ok client_key=%s latency_ms=%lu", p->client_key,
             (unsigned long)p->last_latency_ms);
    return ESP_OK;
}

esp_err_t philips_observe(philips_t *p)
{
    esp_err_t err = send_req(p, COAP_GET, "/sys/dev/status", 0, NULL, 0);
    if (err == ESP_OK) p->observe_sent_us = esp_timer_get_time();
    return err;
}

esp_err_t philips_recv_status(philips_t *p, char *json, size_t json_cap, uint32_t timeout_ms)
{
    coap_msg_t reply;
    esp_err_t err = wait_reply(p, "/sys/dev/status", timeout_ms, &reply);
    if (err != ESP_OK) return err;
    p->since_observe_ms = (uint32_t)((esp_timer_get_time() - p->observe_sent_us) / 1000);
    if (!reply.payload) return ESP_ERR_INVALID_RESPONSE;
    p->max_age = reply.has_max_age ? reply.max_age : 60;
    if (reply.has_observe) p->last_observe = reply.observe;
    size_t len = 0;
    err = philips_decrypt((const char *)reply.payload, reply.payload_len, json, json_cap, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decrypt failed err=%s payload_len=%u", esp_err_to_name(err),
                 (unsigned)reply.payload_len);
        return err;
    }
    ESP_LOGD(TAG, "status payload_hex=%u json=%u observe=%s%lu max_age=%lu since_observe_ms=%lu",
             (unsigned)reply.payload_len, (unsigned)len, reply.has_observe ? "" : "none/",
             (unsigned long)p->last_observe, (unsigned long)p->max_age, (unsigned long)p->since_observe_ms);
    return ESP_OK;
}
