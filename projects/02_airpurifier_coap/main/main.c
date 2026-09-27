// 02_airpurifier_coap — read a Philips air purifier's live status over local CoAP.
// Joins Wi-Fi, syncs with the purifier, registers an Observe on /sys/dev/status and logs
// one `status` line (plus the raw JSON at DEBUG) for the first reply and every change the
// purifier pushes. Silence longer than max_age + 15 s re-registers, which doubles as a
// liveness check; three unanswered re-registers trigger a re-sync.
// Statuses also feed 3-minute buckets (bucket.c). Each window is closed on the 3-minute
// mark and queued for Supabase (uploader.c); a quiet window is carried forward only if a
// sync probe answers, so a gap in the table means offline. A status row goes out every
// 5 minutes. Design: "Cloud logging" in docs/design/02_airpurifier_coap.md.
// Recovery (either device can lose power at any time): the purifier link is re-established
// the same way at boot and after an outage - sync every 10 s, and after 3 failures sweep the
// /24 for it (its DHCP address has moved before) and remember the new IP in NVS. The main
// loop and the uploader are on the task watchdog; a hang or a leak ends in a reboot.
// Serial: 'i' = heartbeat now, 'g' = re-register (fresh status now), 's' = re-sync,
// 'o' = simulate a 10-min Supabase outage, 'w' = drop Wi-Fi for 4 min, 'x' = pretend the
// purifier moved (point at a dead IP; discovery must find it), 'h' = hang the main loop 70 s
// (the watchdog must reboot), 'r' = restart.
// Design: docs/design/02_airpurifier_coap.md.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "bucket.h"
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "net_time.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "philips_coap.h"
#include "philips_crypto.h"
#include "sdkconfig.h"
#include "supabase.h"
#include "uploader.h"
#include "wifi_sta.h"
#include "xiao_board.h"

static const char *TAG = "main";

#define JSON_CAP 3072

static philips_t s_dev;
static char s_device_id[24]; // "xiao-c6-" + last 3 MAC bytes; salts the row ids
static char s_host[16];      // purifier IP: NVS (last discovered) or CONFIG_PURIFIER_HOST
static bool s_linked;        // the purifier answered the last sync
static uint32_t s_sync_fail, s_discoveries, s_host_changes;
static int64_t s_last_discover_us;

#define HEAP_FLOOR_BYTES (30 * 1024) // below this, reboot: a leak ends here, not in a crash
static char s_json[JSON_CAP];
static volatile char s_cmd;

static uint32_t s_updates, s_errors, s_syncs, s_observes, s_unanswered;
static int64_t s_last_ok_us;
// True between an Observe GET and the first status after it. The purifier does not answer
// the GET directly: it registers us and sends at its next push (25-64 s seen), so "first"
// is the first notification, not a reply.
static bool s_awaiting_reply;
static uint32_t s_dups;       // statuses repeating the previous Observe sequence number
static uint32_t s_prev_observe;
static uint32_t s_closed, s_carried, s_skipped; // bucket windows: with samples / carried / no row

// AC22xx "new2" keys (PhilipsAC22xx in kongo09/philips-airpurifier-coap).
static const char *mode_name(int m)
{
    switch (m) {
    case 0: return "auto";
    case 1: case 2: case 3: case 4: case 5: return "speed";
    case 17: return "sleep";
    case 18: return "turbo";
    case 19: return "medium";
    default: return "?";
    }
}

static int jint(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : -1;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "?";
}

static void log_status(const char *json, const char *kind)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "state"), "reported");
    if (!r) {
        ESP_LOGW(TAG, "status json without state.reported len=%u", (unsigned)strlen(json));
    } else {
        int mode = jint(r, "D0310C");
        ESP_LOGI(TAG, "status kind=%s model=%s pwr=%d mode=%d(%s) fan=%d pm25=%d iai=%d err=%d "
                 "prefilter_h=%d/%d hepa_h=%d/%d dev_rssi=%d observe=%" PRIu32 " since_get_ms=%" PRIu32,
                 kind, jstr(r, "D01S05"), jint(r, "D03102"), mode, mode_name(mode), jint(r, "D0310D"),
                 jint(r, "D03221"), jint(r, "D03120"), jint(r, "D03240"), jint(r, "D0520D"),
                 jint(r, "D05207"), jint(r, "D0540E"), jint(r, "D05408"), jint(r, "rssi"),
                 s_dev.last_observe, s_dev.since_observe_ms);
    }
    if (r) bucket_on_status(r, net_time_epoch_ms_now());
    cJSON_Delete(root);
    ESP_LOGD(TAG, "raw %s", json);
}

static void heartbeat(void)
{
    int64_t now = esp_timer_get_time();
    long age = s_last_ok_us ? (long)((now - s_last_ok_us) / 1000000) : -1;
    ESP_LOGI(TAG, "hb up=%" PRIu64 " heap=%" PRIu32 " heap_min=%" PRIu32 " rssi=%d wifi_drops=%" PRIu32
             " updates=%" PRIu32 " errors=%" PRIu32 " observes=%" PRIu32 " syncs=%" PRIu32
             " dups=%" PRIu32 " last_update_age_s=%ld stray=%" PRIu32 " time_synced=%d",
             now / 1000000, esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
             wifi_sta_rssi(), wifi_sta_disconnects(), s_updates, s_errors, s_observes, s_syncs, s_dups, age,
             s_dev.stray, net_time_synced());
    uploader_stats_t u;
    uploader_get_stats(&u);
    ESP_LOGI(TAG, "hb_up sent=%" PRIu32 " dup=%" PRIu32 " fail=%" PRIu32 " dropped=%" PRIu32
             " depth=%" PRIu32 " depth_max=%" PRIu32 " last_ok_age_s=%" PRId32 " closed=%" PRIu32
             " carried=%" PRIu32 " skipped=%" PRIu32,
             u.sent, u.duplicates, u.failures, u.dropped, u.depth, u.depth_max, u.last_ok_age_s,
             s_closed, s_carried, s_skipped);
    ESP_LOGI(TAG, "hb_link linked=%d host=%s sync_fail=%" PRIu32 " discoveries=%" PRIu32 " host_changes=%" PRIu32,
             s_linked, s_host, s_sync_fail, s_discoveries, s_host_changes);
}

static void console_task(void *arg)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    uint8_t c;
    for (;;) {
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        ESP_LOGI(TAG, "cmd '%c'", c);
        if (c == 'i') heartbeat();
        else if (c == 'r') esp_restart();
        else if (c == 'o') uploader_simulate_outage(600);
        else if (c == 'w') wifi_sta_suspend(240);
        else if (c == 'g' || c == 's' || c == 'x' || c == 'h') s_cmd = (char)c; // run by the loop within 1 s
    }
}

static bool do_sync(void)
{
    s_syncs++;
    esp_err_t err = philips_sync(&s_dev);
    if (err == ESP_OK) {
        if (!s_linked) ESP_LOGI(TAG, "purifier linked host=%s after_fails=%" PRIu32, s_host, s_sync_fail);
        s_linked = true;
        s_sync_fail = 0;
    } else {
        if (s_linked) ESP_LOGW(TAG, "purifier lost host=%s", s_host);
        s_linked = false;
        s_sync_fail++;
        ESP_LOGW(TAG, "sync failed err=%s fails=%" PRIu32, esp_err_to_name(err), s_sync_fail);
    }
    return err == ESP_OK;
}

static void load_host(void)
{
    strlcpy(s_host, CONFIG_PURIFIER_HOST, sizeof(s_host));
    nvs_handle_t h;
    if (nvs_open("purifier", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_host);
        char saved[16];
        if (nvs_get_str(h, "host", saved, &len) == ESP_OK && saved[0]) strlcpy(s_host, saved, sizeof(s_host));
        nvs_close(h);
    }
    ESP_LOGI(TAG, "purifier host=%s (config=%s)", s_host, CONFIG_PURIFIER_HOST);
}

static void save_host(void)
{
    nvs_handle_t h;
    if (nvs_open("purifier", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "host", s_host);
        nvs_commit(h);
        nvs_close(h);
    }
}

// The purifier stopped answering at s_host. It may be off, or back on with a new DHCP
// address: sweep the /24 at most every 5 minutes, and move if it turns up elsewhere.
static void maybe_discover(void)
{
    int64_t now = esp_timer_get_time();
    if (!wifi_sta_connected() || s_sync_fail < 3) return;
    if (s_last_discover_us && now - s_last_discover_us < 300LL * 1000000) return;
    s_last_discover_us = now;
    s_discoveries++;
    char found[16];
    esp_task_wdt_reset(); // discovery can take ~15 s in the sweep fallback
    esp_err_t d = philips_discover(CONFIG_PURIFIER_PORT, 2000, found, sizeof(found));
    esp_task_wdt_reset();
    if (d != ESP_OK) return;
    if (strcmp(found, s_host) != 0) {
        ESP_LOGW(TAG, "purifier moved %s -> %s", s_host, found);
        strlcpy(s_host, found, sizeof(s_host));
        save_host();
        s_host_changes++;
        philips_close(&s_dev);
        philips_open(&s_dev, s_host, CONFIG_PURIFIER_PORT);
    }
}

static void do_observe(const char *why)
{
    s_observes++;
    s_awaiting_reply = true;
    esp_err_t err = philips_observe(&s_dev);
    ESP_LOGI(TAG, "observe why=%s n=%" PRIu32 " send_us=%" PRIu32 " err=%s", why, s_observes,
             s_dev.send_us, esp_err_to_name(err));
}

// Close the window ending at end_ms and queue its row. With samples: always. Quiet: only
// if the purifier answers a sync probe now (status can't be fetched on demand, sync can).
static void close_window(int64_t end_ms)
{
    reading_row_t row;
    int n = bucket_samples(end_ms);
    bool carry = false;
    if (n == 0 && bucket_state()->valid && s_linked) carry = do_sync();
    if (!bucket_close(end_ms, carry, &row)) {
        s_skipped++;
        ESP_LOGW(TAG, "window end_ms=%lld no row (samples=0 purifier_answered=%d state=%d)",
                 (long long)end_ms, carry, bucket_state()->valid);
        return;
    }
    if (n) s_closed++;
    else s_carried++;
    ESP_LOGI(TAG, "window end_ms=%lld samples=%d pm25_mean=%.1f max=%d queued", (long long)end_ms,
             row.n_samples, row.pm25_mean, row.pm25_max);
    uploader_push_reading(&row);
}

static void queue_status(int64_t ts_ms)
{
    const purifier_state_t *st = bucket_state();
    status_row_t s = {
        .ts_ms = ts_ms,
        .free_heap = esp_get_free_heap_size(),
        .min_heap = esp_get_minimum_free_heap_size(),
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
        .wifi_rssi = wifi_sta_rssi(),
        .wifi_drops = wifi_sta_disconnects(),
        .observes = s_observes,
        .syncs = s_syncs,
        .updates = s_updates,
        .last_update_age_s = s_last_ok_us ? (int32_t)((esp_timer_get_time() - s_last_ok_us) / 1000000) : -1,
    };
    if (st->valid) {
        strlcpy(s.purifier_id, st->purifier_id, sizeof(s.purifier_id));
        strlcpy(s.purifier_name, st->name, sizeof(s.purifier_name));
        strlcpy(s.purifier_model, st->model, sizeof(s.purifier_model));
        strlcpy(s.purifier_fw, st->fw, sizeof(s.purifier_fw));
    }
    uploader_push_status(&s);
}

void app_main(void)
{
    static const char *const own_tags[] = {"main", "philips_coap", "philips_crypto", "bucket", "net_time",
                                           "supabase", "wifi_sta", "xiao_board"};
    for (size_t i = 0; i < sizeof(own_tags) / sizeof(own_tags[0]); i++) {
        esp_log_level_set(own_tags[i], ESP_LOG_DEBUG);
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "xiao-c6-%02x%02x%02x", mac[3], mac[4], mac[5]);
    const esp_app_desc_t *app = esp_app_get_description();
    // built= is esp_app_desc.c's compile time and only changes on a full rebuild; the ELF
    // hash changes on every build, so it's what tells two flashes apart in a log.
    char elf[9];
    esp_app_get_elf_sha256(elf, sizeof(elf));
    ESP_LOGI(TAG, "boot project=%s built=%s_%s idf=%s elf=%s reset=%d host=%s device_id=%s supabase=%d", app->project_name,
             app->date, app->time, app->idf_ver, elf, esp_reset_reason(), CONFIG_PURIFIER_HOST, s_device_id,
             supabase_configured());
    ESP_ERROR_CHECK(xiao_board_led_init());
    xTaskCreate(console_task, "console", 3072, NULL, 5, NULL);

    esp_err_t st = philips_crypto_selftest();
    ESP_LOGI(TAG, "selftest crypto=%s", st == ESP_OK ? "ok" : "FAIL");

    esp_err_t err = wifi_sta_start(20000);
    ESP_LOGI(TAG, "selftest wifi=%s rssi=%d", err == ESP_OK ? "ok" : "FAIL", wifi_sta_rssi());
    net_time_start();
    uploader_start(s_device_id);

    load_host();
    if (!s_host[0] || philips_open(&s_dev, s_host, CONFIG_PURIFIER_PORT) != ESP_OK) {
        // No host at all: discovery will find one once the sync-failure path runs.
        ESP_LOGW(TAG, "no usable purifier host yet - will discover");
        s_host[0] = '\0';
        s_sync_fail = 3;
    } else if (do_sync()) {
        do_observe("start");
    }

    // From here the loop never blocks longer than a sync (12 s) plus a discovery (~4 s)
    // between feeds (the window-closing loop feeds too); 60 s of silence means it's stuck.
    esp_err_t wdt = esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "task_wdt main add=%s status=%s", esp_err_to_name(wdt), esp_err_to_name(esp_task_wdt_status(NULL)));
    int64_t last_hb = esp_timer_get_time();
    int64_t last_rx = esp_timer_get_time(); // last status, or last (re-)register
    // Last window closed, and last 5-minute status mark. 0 = start from the first window
    // seen after the clock syncs and a status arrives; nothing before that is emitted.
    int64_t closed_end = 0, status_mark = 0;
    int64_t last_link_try = 0;
    for (;;) {
        esp_task_wdt_reset();
        char cmd = s_cmd;
        s_cmd = 0;
        int64_t tnow = esp_timer_get_time();
        if (!s_linked && tnow - last_link_try >= 10LL * 1000000) {
            last_link_try = tnow;
            maybe_discover();
            if (s_host[0] && do_sync()) do_observe("relink");
        }
        if (esp_get_free_heap_size() < HEAP_FLOOR_BYTES) {
            ESP_LOGE(TAG, "heap %" PRIu32 " below floor - restarting", esp_get_free_heap_size());
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        if (cmd == 's' && do_sync()) do_observe("cmd_sync");
        if (cmd == 'g') do_observe("cmd");
        if (cmd == 'x') {
            // Nothing answers CoAP at .254 (the /24 sweep that found the purifier got one reply).
            ESP_LOGW(TAG, "test: pretending the purifier moved - host %s -> 192.168.1.254", s_host);
            strlcpy(s_host, "192.168.1.254", sizeof(s_host));
            save_host();
            philips_close(&s_dev);
            philips_open(&s_dev, s_host, CONFIG_PURIFIER_PORT);
            s_linked = false;
            s_last_discover_us = 0;
        }
        if (cmd == 'h') {
            ESP_LOGW(TAG, "test: hanging the main loop for 70 s - the task watchdog should reboot us (subscribed=%s)",
                     esp_err_to_name(esp_task_wdt_status(NULL)));
            vTaskDelay(pdMS_TO_TICKS(70000));
            ESP_LOGE(TAG, "test: still alive after the hang - the watchdog did NOT fire");
        }

        esp_err_t err = ESP_ERR_TIMEOUT;
        if (s_dev.sock >= 0) {
            err = philips_recv_status(&s_dev, s_json, sizeof(s_json), 1000);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000)); // no host yet: nothing to listen to
        }
        int64_t now = esp_timer_get_time();
        if (err == ESP_OK && s_updates > 0 && s_dev.last_observe == s_prev_observe) {
            // Same notification twice: a re-register left a second observer behind.
            s_dups++;
            ESP_LOGW(TAG, "status dup observe=%" PRIu32 " dups=%" PRIu32, s_dev.last_observe, s_dups);
            s_last_ok_us = last_rx = now;
        } else if (err == ESP_OK) {
            s_prev_observe = s_dev.last_observe;
            s_updates++;
            s_unanswered = 0;
            s_last_ok_us = last_rx = now;
            xiao_board_led_set(true);
            log_status(s_json, s_awaiting_reply ? "first" : "notify");
            s_awaiting_reply = false;
            xiao_board_led_set(false);
        } else if (err != ESP_ERR_TIMEOUT) {
            s_errors++;
            ESP_LOGW(TAG, "status error err=%s", esp_err_to_name(err));
        } else if (now - last_rx > (int64_t)(s_dev.max_age + 15) * 1000000) {
            // Quiet: the purifier only pushes on change. Re-registering gets an immediate
            // status back, or tells us it is gone.
            if (s_awaiting_reply) s_unanswered++;
            if (s_unanswered >= 3) {
                ESP_LOGW(TAG, "no reply to %" PRIu32 " observes - re-syncing", s_unanswered);
                s_unanswered = 0;
                do_sync(); // on failure s_linked drops and the relink path takes over
            }
            if (s_linked) do_observe(s_awaiting_reply ? "no_reply" : "quiet");
            last_rx = now;
        }
        int64_t epoch = net_time_epoch_ms_now();
        if (epoch > 0 && bucket_state()->valid) {
            int64_t prev_end = bucket_end_for(epoch) - BUCKET_MS; // the latest closed window
            if (closed_end == 0) {
                closed_end = prev_end; // the window we're in now is the first one emitted
            } else if (prev_end > closed_end) {
                // Normally one window; more only if the loop was held up (a long sync).
                for (int64_t e = closed_end + BUCKET_MS; e <= prev_end; e += BUCKET_MS) {
                    esp_task_wdt_reset(); // each close may spend a 12 s sync
                    close_window(e);
                }
                closed_end = prev_end;
            }
        }
        if (epoch > 0) {
            int64_t mark = epoch / 300000 * 300000;
            if (mark != status_mark) {
                if (status_mark) queue_status(mark);
                status_mark = mark;
            }
        }
        if (now - last_hb >= 30LL * 1000000) {
            last_hb = now;
            heartbeat();
        }
    }
}
