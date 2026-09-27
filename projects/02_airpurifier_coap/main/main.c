// 02_airpurifier_coap — read a Philips air purifier's live status over local CoAP.
// Joins Wi-Fi, syncs with the purifier, registers an Observe on /sys/dev/status and logs
// one `status` line (plus the raw JSON at DEBUG) for the first reply and every change the
// purifier pushes. Silence longer than max_age + 15 s re-registers, which doubles as a
// liveness check; three unanswered re-registers trigger a re-sync.
// Serial: 'i' = heartbeat now, 'g' = re-register (fresh status now), 's' = re-sync, 'r' = restart.
// Design: docs/design/02_airpurifier_coap.md.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "philips_coap.h"
#include "philips_crypto.h"
#include "sdkconfig.h"
#include "wifi_sta.h"
#include "xiao_board.h"

static const char *TAG = "main";

#define JSON_CAP 3072

static philips_t s_dev;
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
    cJSON_Delete(root);
    ESP_LOGD(TAG, "raw %s", json);
}

static void heartbeat(void)
{
    int64_t now = esp_timer_get_time();
    long age = s_last_ok_us ? (long)((now - s_last_ok_us) / 1000000) : -1;
    ESP_LOGI(TAG, "hb up=%" PRIu64 " heap=%" PRIu32 " heap_min=%" PRIu32 " rssi=%d wifi_drops=%" PRIu32
             " updates=%" PRIu32 " errors=%" PRIu32 " observes=%" PRIu32 " syncs=%" PRIu32
             " dups=%" PRIu32 " last_update_age_s=%ld stray=%" PRIu32,
             now / 1000000, esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
             wifi_sta_rssi(), wifi_sta_disconnects(), s_updates, s_errors, s_observes, s_syncs, s_dups, age,
             s_dev.stray);
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
        else if (c == 'g' || c == 's') s_cmd = (char)c; // picked up within 1 s by the loop
    }
}

static bool do_sync(void)
{
    s_syncs++;
    esp_err_t err = philips_sync(&s_dev);
    if (err != ESP_OK) ESP_LOGW(TAG, "sync failed err=%s", esp_err_to_name(err));
    return err == ESP_OK;
}

static void do_observe(const char *why)
{
    s_observes++;
    s_awaiting_reply = true;
    esp_err_t err = philips_observe(&s_dev);
    ESP_LOGI(TAG, "observe why=%s n=%" PRIu32 " send_us=%" PRIu32 " err=%s", why, s_observes,
             s_dev.send_us, esp_err_to_name(err));
}

void app_main(void)
{
    static const char *const own_tags[] = {"main", "philips_coap", "philips_crypto", "wifi_sta", "xiao_board"};
    for (size_t i = 0; i < sizeof(own_tags) / sizeof(own_tags[0]); i++) {
        esp_log_level_set(own_tags[i], ESP_LOG_DEBUG);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "boot project=%s built=%s_%s idf=%s reset=%d host=%s", app->project_name,
             app->date, app->time, app->idf_ver, esp_reset_reason(), CONFIG_PURIFIER_HOST);
    ESP_ERROR_CHECK(xiao_board_led_init());
    xTaskCreate(console_task, "console", 3072, NULL, 5, NULL);

    esp_err_t st = philips_crypto_selftest();
    ESP_LOGI(TAG, "selftest crypto=%s", st == ESP_OK ? "ok" : "FAIL");

    esp_err_t err = wifi_sta_start(20000);
    ESP_LOGI(TAG, "selftest wifi=%s rssi=%d", err == ESP_OK ? "ok" : "FAIL", wifi_sta_rssi());

    if (strlen(CONFIG_PURIFIER_HOST) == 0 ||
        philips_open(&s_dev, CONFIG_PURIFIER_HOST, CONFIG_PURIFIER_PORT) != ESP_OK) {
        ESP_LOGE(TAG, "no usable CONFIG_PURIFIER_HOST - set it in sdkconfig.defaults.local");
        for (;;) {
            heartbeat();
            vTaskDelay(pdMS_TO_TICKS(30000));
        }
    }

    while (!do_sync()) {
        heartbeat();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    do_observe("start");

    int64_t last_hb = esp_timer_get_time();
    int64_t last_rx = esp_timer_get_time(); // last status, or last (re-)register
    for (;;) {
        char cmd = s_cmd;
        s_cmd = 0;
        if (cmd == 's' && do_sync()) do_observe("cmd_sync");
        if (cmd == 'g') do_observe("cmd");

        esp_err_t err = philips_recv_status(&s_dev, s_json, sizeof(s_json), 1000);
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
                do_sync();
            }
            do_observe(s_awaiting_reply ? "no_reply" : "quiet");
            last_rx = now;
        }
        if (now - last_hb >= 30LL * 1000000) {
            last_hb = now;
            heartbeat();
        }
    }
}
