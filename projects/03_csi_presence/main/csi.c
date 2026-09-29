#include "csi.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"

static const char *TAG = "csi";

#define CSI_MAX_BYTES 1024           // rx_channel_estimate_len is 10 bits
#define CSI_MAX_SUB (CSI_MAX_BYTES / 2)
#define QUEUE_LEN 12
#define RELOCK_MIN 10                // mismatched frames in a window with no match
#define CSI_TASK_PRIO 6
#define PING_TASK_PRIO 7             // above the CSI task: a busy CSI task drops frames (counted), never slows the source
#define RAW_TASK_PRIO 3
#define RAW_LINE_MAX (160 + (CSI_MAX_BYTES + 2) / 3 * 4) // header + base64 of the I/Q bytes
#define RAW_BUF_BYTES (12 * 1024)    // ~14 lines at len=512

typedef struct {
    uint32_t ts;
    uint32_t siga1;    // HE-SIG-A1 / HT-SIG (MCS, bandwidth, GI), kept for offline sorting
    uint16_t sig_len;  // MPDU length: tells ping replies from other frames
    int8_t rssi;
    int8_t nf;
    uint8_t fmt;
    uint8_t rate;
    bool group;
    bool first_invalid;
    uint16_t len;
    int8_t buf[CSI_MAX_BYTES];
} frame_t;

static QueueHandle_t s_queue;
static MessageBufferHandle_t s_raw_buf;
static csi_second_cb_t s_on_second;
static uint8_t s_bssid[6];
static volatile bool s_raw;
static uint32_t s_raw_seq;
static csi_stats_t s_stats; // counters written by the Wi-Fi task and the CSI task only
static frame_t s_scratch;   // the Wi-Fi task's copy buffer (one callback at a time)

// Window accumulators: per-subcarrier sums of the normalised amplitude
static float s_sum[CSI_MAX_SUB];
static float s_sum2[CSI_MAX_SUB];
static float s_amp[CSI_MAX_SUB];

static void csi_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf || info->len == 0) return;
    if (memcmp(info->mac, s_bssid, 6) != 0) {
        s_stats.other++;
        return;
    }
    uint16_t len = info->len;
    if (len > CSI_MAX_BYTES) {
        s_stats.trunc++;
        len = CSI_MAX_BYTES;
    }
    s_scratch.ts = info->rx_ctrl.timestamp;
    s_scratch.siga1 = info->rx_ctrl.he_siga1;
    s_scratch.sig_len = info->rx_ctrl.sig_len;
    s_scratch.rssi = info->rx_ctrl.rssi;
    s_scratch.nf = info->rx_ctrl.noise_floor;
    s_scratch.fmt = info->rx_ctrl.cur_bb_format;
    s_scratch.rate = info->rx_ctrl.rate;
    s_scratch.group = info->rx_ctrl.is_group;
    s_scratch.first_invalid = info->first_word_invalid;
    s_scratch.len = len;
    memcpy(s_scratch.buf, info->buf, len);
    if (xQueueSend(s_queue, &s_scratch, 0) != pdTRUE) s_stats.dropped++;
}

// |H_k| for each subcarrier, normalised by the frame's mean over non-null tones.
// Returns the subcarrier count, or 0 if the frame is all nulls.
static int amplitudes(const frame_t *f, float *amp)
{
    int n = f->len / 2;
    int start = f->first_invalid ? 2 : 0; // 4 invalid bytes = 2 subcarriers
    float total = 0;
    int nonzero = 0;
    for (int k = 0; k < n; k++) {
        if (k < start) {
            amp[k] = 0;
            continue;
        }
        float im = f->buf[2 * k], re = f->buf[2 * k + 1];
        amp[k] = sqrtf(im * im + re * re);
        if (amp[k] > 0) {
            total += amp[k];
            nonzero++;
        }
    }
    if (nonzero == 0 || total <= 0) return 0;
    float mean = total / nonzero;
    for (int k = 0; k < n; k++) amp[k] /= mean;
    return n;
}

static int base64(const uint8_t *in, int n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) |
                     (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

// One line per AP frame: metadata plus the exact I/Q bytes (int8 pairs, imaginary first) in
// base64. Formatted here, printed by raw_task, so a slow or absent reader never stalls the
// scoring; s= has a gap wherever a line was dropped.
static void raw_dump(const frame_t *f)
{
    static char line[RAW_LINE_MAX];
    int p = snprintf(line, sizeof(line),
                     "raw s=%" PRIu32 " t=%" PRIu32 " rssi=%d nf=%d fmt=%u rate=%u sl=%u g=%d"
                     " siga1=%08" PRIx32 " fi=%d len=%u iq=",
                     s_raw_seq++, f->ts, f->rssi, f->nf, f->fmt, f->rate, f->sig_len, f->group,
                     f->siga1, f->first_invalid, f->len);
    p += base64((const uint8_t *)f->buf, f->len, line + p);
    if (xMessageBufferSend(s_raw_buf, line, p, 0) == (size_t)p) s_stats.raw_lines++;
    else s_stats.raw_drop++;
}

static void raw_task(void *arg)
{
    static char line[RAW_LINE_MAX + 1];
    for (;;) {
        size_t n = xMessageBufferReceive(s_raw_buf, line, RAW_LINE_MAX, portMAX_DELAY);
        if (n == 0) continue;
        line[n++] = '\n';
        fwrite(line, 1, n, stdout);
        fflush(stdout);
    }
}

// Mean over subcarriers of std/mean across the window, x1000.
static float window_motion(int n_sub, uint32_t n)
{
    if (n < 2) return 0;
    float acc = 0;
    int used = 0;
    for (int k = 0; k < n_sub; k++) {
        float mean = s_sum[k] / n;
        if (mean < 0.05f) continue; // null / guard tone
        float var = s_sum2[k] / n - mean * mean;
        if (var < 0) var = 0;
        acc += sqrtf(var) / mean;
        used++;
    }
    return used ? 1000.0f * acc / used : 0;
}

static void csi_task(void *arg)
{
    static frame_t f;
    uint16_t cand_len = 0;  // most recent mismatched length, for re-locking
    uint32_t win_n = 0, win_mismatch = 0;
    int32_t rssi_acc = 0;
    int last_nf = 0;
    int64_t win_start = esp_timer_get_time();

    for (;;) {
        if (xQueueReceive(s_queue, &f, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_raw) raw_dump(&f);
            if (s_stats.lock_len == 0) {
                s_stats.lock_len = f.len;
                ESP_LOGI(TAG, "csi_lock len=%u fmt=%u n_sub=%u src=" MACSTR, f.len, f.fmt,
                         f.len / 2, MAC2STR(s_bssid));
            }
            if (f.len != s_stats.lock_len) {
                s_stats.skip_len++;
                win_mismatch++;
                cand_len = f.len;
            } else {
                int n = amplitudes(&f, s_amp);
                if (n > 0) {
                    for (int k = 0; k < n; k++) {
                        s_sum[k] += s_amp[k];
                        s_sum2[k] += s_amp[k] * s_amp[k];
                    }
                    win_n++;
                    rssi_acc += f.rssi;
                    last_nf = f.nf;
                    s_stats.frames++;
                    s_stats.last_frame_us = esp_timer_get_time();
                }
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - win_start < 1000000) continue;
        win_start = now;

        csi_second_t s = {
            .motion = window_motion(s_stats.lock_len / 2, win_n),
            .n = win_n,
            .rssi = win_n ? (int)(rssi_acc / (int32_t)win_n) : 0,
            .nf = last_nf,
        };
        if (win_n == 0 && win_mismatch >= RELOCK_MIN && cand_len) {
            s_stats.relocks++;
            ESP_LOGW(TAG, "csi_relock from=%u to=%u mismatched=%" PRIu32, s_stats.lock_len,
                     cand_len, win_mismatch);
            s_stats.lock_len = cand_len;
        }
        memset(s_sum, 0, sizeof(s_sum));
        memset(s_sum2, 0, sizeof(s_sum2));
        win_n = 0;
        win_mismatch = 0;
        rssi_acc = 0;

        // Follow a roam to another AP
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && memcmp(ap.bssid, s_bssid, 6) != 0) {
            ESP_LOGW(TAG, "bssid_change new=" MACSTR, MAC2STR(ap.bssid));
            memcpy(s_bssid, ap.bssid, 6);
            s_stats.lock_len = 0;
        }

        if (s_on_second) s_on_second(&s);
    }
}

static esp_err_t start_ping(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.gw.addr == 0) {
        ESP_LOGE(TAG, "no gateway to ping");
        return ESP_ERR_INVALID_STATE;
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = CONFIG_CSI_PING_INTERVAL_MS;
    // esp_ping waits out the timeout on a lost reply, then vTaskDelayUntil catches up with
    // back-to-back pings. At 1000 ms that was a 1 s gap followed by a ~50-frame burst that
    // overflowed the queue (2026-09-29). A late reply still gives a CSI frame.
    cfg.timeout_ms = 100;
    cfg.data_size = 8;
    cfg.task_stack_size = 3072;
    cfg.task_prio = PING_TASK_PRIO;
    cfg.target_addr.type = ESP_IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = ip.gw.addr;
    esp_ping_callbacks_t cbs = {0};
    esp_ping_handle_t h;
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &h);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "ping target=gw interval_ms=%d", CONFIG_CSI_PING_INTERVAL_MS);
    return esp_ping_start(h);
}

esp_err_t csi_start(csi_second_cb_t on_second)
{
    s_on_second = on_second;
    wifi_ap_record_t ap;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) return err;
    memcpy(s_bssid, ap.bssid, 6);

    s_queue = xQueueCreate(QUEUE_LEN, sizeof(frame_t));
    s_raw_buf = xMessageBufferCreate(RAW_BUF_BYTES);
    if (!s_queue || !s_raw_buf) return ESP_ERR_NO_MEM;
    if (xTaskCreate(csi_task, "csi", 4096, NULL, CSI_TASK_PRIO, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(raw_task, "csi_raw", 3072, NULL, RAW_TASK_PRIO, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    wifi_csi_config_t cfg = {
        .enable = 1,
        .acquire_csi_legacy = 1,
        .acquire_csi_ht20 = 1,
        .acquire_csi_ht40 = 1,
        .acquire_csi_su = 1,
        .acquire_csi_mu = 1,
        .acquire_csi_dcm = 1,
        .acquire_csi_beamformed = 1,
        .acquire_csi_he_stbc = 2,
        .val_scale_cfg = 0,
        .dump_ack_en = 0,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi_config(&cfg), TAG, "csi config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi_rx_cb(csi_cb, NULL), TAG, "csi cb");
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi(true), TAG, "csi enable");
    ESP_LOGI(TAG, "csi_on src=" MACSTR " primary_ch=%u", MAC2STR(s_bssid), ap.primary);
    return start_ping();
}

void csi_get_stats(csi_stats_t *out)
{
    *out = s_stats;
}

void csi_set_raw_dump(bool on)
{
    s_raw = on;
}

bool csi_raw_dump(void)
{
    return s_raw;
}
