#include "uploader.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "supabase.h"
#include "uuid7.h"
#include "wifi_sta.h"

static const char *TAG = "uploader";

typedef struct {
    bool is_status;
    union {
        reading_row_t r;
        status_row_t s;
    };
} item_t;

static item_t s_ring[UPLOADER_RING];
static int s_head, s_count; // oldest at s_head
static SemaphoreHandle_t s_mux;
static TaskHandle_t s_task;
static char s_device_id[24];
static uploader_stats_t s_stats;
static int64_t s_last_ok_us;
static volatile int64_t s_outage_until_us;

static void push(const item_t *it)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_count == UPLOADER_RING) {
        // Full: the oldest row is lost. Accepted by design (6 h of tolerance), but counted.
        s_head = (s_head + 1) % UPLOADER_RING;
        s_count--;
        s_stats.dropped++;
        ESP_LOGW(TAG, "ring full - oldest row dropped dropped=%" PRIu32, s_stats.dropped);
    }
    s_ring[(s_head + s_count) % UPLOADER_RING] = *it;
    s_count++;
    if ((uint32_t)s_count > s_stats.depth_max) s_stats.depth_max = s_count;
    xSemaphoreGive(s_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

void uploader_push_reading(const reading_row_t *r)
{
    item_t it = {.is_status = false, .r = *r};
    push(&it);
}

void uploader_push_status(const status_row_t *s)
{
    item_t it = {.is_status = true, .s = *s};
    push(&it);
}

void uploader_get_stats(uploader_stats_t *out)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_stats;
    out->depth = s_count;
    out->last_ok_age_s = s_last_ok_us ? (int32_t)((esp_timer_get_time() - s_last_ok_us) / 1000000) : -1;
    xSemaphoreGive(s_mux);
}

void uploader_simulate_outage(uint32_t seconds)
{
    s_outage_until_us = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    ESP_LOGW(TAG, "simulated supabase outage %s for %" PRIu32 " s", seconds ? "ON" : "OFF", seconds);
    if (s_task) xTaskNotifyGive(s_task);
}

static void iso8601(int64_t ms, char *buf, size_t cap)
{
    time_t secs = (time_t)(ms / 1000);
    struct tm utc;
    gmtime_r(&secs, &utc);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

// -1 (a key this model doesn't report) goes out as JSON null.
static const char *num(char *buf, int v)
{
    if (v < 0) return "null";
    snprintf(buf, 12, "%d", v);
    return buf;
}

static int render_reading(const reading_row_t *r, char *out, size_t cap)
{
    char id[UUID7_STR_LEN], ts[24], mean[16], a[10][12];
    uuid7_deterministic(r->end_ms, s_device_id, r->purifier_id, strlen(r->purifier_id), id);
    iso8601(r->end_ms, ts, sizeof(ts));
    if (r->pm25_mean < 0) strcpy(mean, "null");
    else snprintf(mean, sizeof(mean), "%.2f", r->pm25_mean);
    return snprintf(out, cap,
        "[{\"id\":\"%s\",\"ts\":\"%s\",\"device_id\":\"%s\",\"purifier_id\":\"%s\","
        "\"pm25_mean\":%s,\"pm25_min\":%s,\"pm25_max\":%s,\"iai_max\":%s,\"pwr\":%s,\"mode\":%s,"
        "\"fan\":%s,\"err\":%s,\"prefilter_h\":%s,\"hepa_h\":%s,\"dev_rssi\":%s,\"n_samples\":%d}]",
        id, ts, s_device_id, r->purifier_id, mean, num(a[0], r->pm25_min), num(a[1], r->pm25_max),
        num(a[2], r->iai_max), num(a[3], r->pwr), num(a[4], r->mode), num(a[5], r->fan),
        num(a[6], r->err), num(a[7], r->prefilter_h), num(a[8], r->hepa_h),
        r->dev_rssi ? (snprintf(a[9], 12, "%d", r->dev_rssi), a[9]) : "null", r->n_samples);
}

// A NULL or empty string goes out as JSON null. Purifier strings come from its own status
// (name set in the Philips app); quotes and backslashes are dropped rather than escaped.
static const char *jstr(char *buf, size_t cap, const char *s)
{
    if (!s || !*s) return "null";
    size_t o = 0;
    buf[o++] = '"';
    for (; *s && o < cap - 2; s++) {
        if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20) continue;
        buf[o++] = *s;
    }
    buf[o++] = '"';
    buf[o] = '\0';
    return buf;
}

static int render_status(const status_row_t *s, char *out, size_t cap)
{
    char id[UUID7_STR_LEN], ts[24], age[12], p[4][48];
    // Source "status" keeps these ids apart from reading ids with the same ms.
    uuid7_deterministic(s->ts_ms, s_device_id, "status", 6, id);
    iso8601(s->ts_ms, ts, sizeof(ts));
    return snprintf(out, cap,
        "[{\"id\":\"%s\",\"ts\":\"%s\",\"device_id\":\"%s\",\"purifier_id\":%s,\"purifier_name\":%s,"
        "\"purifier_model\":%s,\"purifier_fw\":%s,\"free_heap\":%" PRIu32 ",\"min_heap\":%" PRIu32 ","
        "\"uptime_s\":%" PRIu32 ",\"wifi_rssi\":%d,\"wifi_drops\":%" PRIu32 ",\"observes\":%" PRIu32 ","
        "\"syncs\":%" PRIu32 ",\"updates\":%" PRIu32 ",\"last_update_age_s\":%s,"
        "\"rows_sent\":%" PRIu32 ",\"upload_fail\":%" PRIu32 "}]",
        id, ts, s_device_id, jstr(p[0], 48, s->purifier_id), jstr(p[1], 48, s->purifier_name),
        jstr(p[2], 48, s->purifier_model), jstr(p[3], 48, s->purifier_fw), s->free_heap, s->min_heap,
        s->uptime_s, s->wifi_rssi, s->wifi_drops, s->observes, s->syncs, s->updates,
        s->last_update_age_s < 0 ? "null" : (snprintf(age, sizeof(age), "%" PRId32, s->last_update_age_s), age),
        s_stats.sent, s_stats.failures);
}

static void upload_task(void *arg)
{
    static char json[1024];
    uint32_t backoff_ms = 5000;
    // Waits below are <= 30 s, an HTTP attempt <= ~15 s; 60 s = stuck.
    ESP_LOGI(TAG, "task_wdt uploader add=%s", esp_err_to_name(esp_task_wdt_add(NULL)));
    for (;;) {
        esp_task_wdt_reset();
        xSemaphoreTake(s_mux, portMAX_DELAY);
        bool have = s_count > 0;
        item_t it;
        if (have) it = s_ring[s_head];
        xSemaphoreGive(s_mux);
        if (!have || !wifi_sta_connected()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(have ? 2000 : 30000)); // <= 30 s: watchdog
            continue;
        }

        int len = it.is_status ? render_status(&it.s, json, sizeof(json))
                               : render_reading(&it.r, json, sizeof(json));
        const char *table = it.is_status ? "purifier_device_status" : "purifier_reading";
        if (esp_timer_get_time() < s_outage_until_us) table = "purifier_outage_test_no_such_table";
        supa_result_t res = SUPA_FAILED;
        if (len > 0 && (size_t)len < sizeof(json)) {
            res = supabase_insert(table, json, 1);
        } else {
            ESP_LOGE(TAG, "row render failed len=%d - dropping it", len);
            res = SUPA_DUPLICATE; // unrecoverable: take it off the ring rather than loop forever
        }

        if (res == SUPA_FAILED) {
            s_stats.failures++;
            ESP_LOGW(TAG, "upload failed depth=%d retry_in_ms=%" PRIu32, s_count, backoff_ms);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms * 2 > 30000 ? 30000 : backoff_ms * 2; // <= 30 s: watchdog
            continue;
        }
        backoff_ms = 5000;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        // The main task may have dropped the oldest while we were posting (ring full);
        // pop only if the head is still the item we sent.
        if (s_count && memcmp(&s_ring[s_head], &it, sizeof(it)) == 0) {
            s_head = (s_head + 1) % UPLOADER_RING;
            s_count--;
        }
        if (res == SUPA_OK) s_stats.sent++;
        else s_stats.duplicates++;
        s_last_ok_us = esp_timer_get_time();
        int depth = s_count;
        xSemaphoreGive(s_mux);
        ESP_LOGI(TAG, "stored kind=%s result=%s depth=%d", it.is_status ? "status" : "reading",
                 supa_result_str(res), depth);
    }
}

void uploader_start(const char *device_id)
{
    strlcpy(s_device_id, device_id, sizeof(s_device_id));
    s_mux = xSemaphoreCreateMutex();
    // TLS handshake needs the stack; 8 KB is what the main task needed in C2.
    xTaskCreate(upload_task, "uploader", 8192, NULL, 4, &s_task);
    ESP_LOGI(TAG, "start device_id=%s ring=%d configured=%d", s_device_id, UPLOADER_RING,
             supabase_configured());
}
