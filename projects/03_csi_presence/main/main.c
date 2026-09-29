// 03_csi_presence — room presence from Wi-Fi channel state information.
// Pings the gateway so every reply is a CSI frame, scores per-second amplitude motion and
// keeps a present/absent state with hysteresis. Design: docs/design/03_csi_presence.md.
// Serial: 'i' status, 'r' restart, 'b' calibrate a 30 s empty-room baseline,
// 'c' toggle raw per-frame amplitude dump, 'm' toggle the 1 Hz motion lines.
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include "csi.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "wifi_sta.h"
#include "xiao_board.h"

static const char *TAG = "main";

#define CAL_SECONDS 30

static volatile bool s_motion_lines = true;
static float s_thr = CONFIG_CSI_DEFAULT_THRESHOLD;
static bool s_cal;
static bool s_present;
static uint8_t s_recent;      // bit history of the last 3 seconds above threshold
static int64_t s_last_above_us;
static int64_t s_state_since_us;
static float s_last_motion;

// Calibration: collected in the CSI task, triggered from the console task
static volatile int s_cal_left;
static float s_cal_sum, s_cal_sum2;
static int s_cal_n;

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    default: return "other";
    }
}

static void log_boot(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "boot project=%s built=%s_%s idf=%s reset=%s", app->project_name, app->date,
             app->time, app->idf_ver, reset_reason_str(esp_reset_reason()));
    ESP_LOGI(TAG, "chip model=%d rev=%d.%d flash_mb=%" PRIu32 " mac=" MACSTR, chip.model,
             chip.revision / 100, chip.revision % 100, flash_bytes / (1024 * 1024), MAC2STR(mac));
}

// Threshold is stored as an integer x100 in NVS namespace "csi"
static void load_threshold(void)
{
    nvs_handle_t h;
    if (nvs_open("csi", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t v;
    if (nvs_get_u32(h, "thr_x100", &v) == ESP_OK && v > 0) {
        s_thr = v / 100.0f;
        s_cal = true;
    }
    nvs_close(h);
}

static void save_threshold(void)
{
    nvs_handle_t h;
    if (nvs_open("csi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "thr_x100", (uint32_t)lroundf(s_thr * 100));
    nvs_commit(h);
    nvs_close(h);
}

static void log_status(void)
{
    csi_stats_t st;
    csi_get_stats(&st);
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "hb up=%" PRIu64 " heap=%" PRIu32 " heap_min=%" PRIu32 " rssi=%d wifi=%d"
             " disconnects=%" PRIu32 " frames=%" PRIu32 " other=%" PRIu32 " dropped=%" PRIu32
             " skip_len=%" PRIu32 " trunc=%" PRIu32 " relocks=%" PRIu32 " lock_len=%u"
             " last_frame_ms=%" PRId64 " motion=%.1f thr=%.1f cal=%d present=%d",
             now / 1000000, esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
             wifi_sta_rssi(), wifi_sta_connected(), wifi_sta_disconnects(), st.frames, st.other,
             st.dropped, st.skip_len, st.trunc, st.relocks, st.lock_len,
             st.last_frame_us ? (now - st.last_frame_us) / 1000 : -1, s_last_motion, s_thr, s_cal,
             s_present);
}

static void on_second(const csi_second_t *s)
{
    int64_t now = esp_timer_get_time();
    s_last_motion = s->motion;

    if (s_cal_left > 0 && s->n > 0) {
        s_cal_sum += s->motion;
        s_cal_sum2 += s->motion * s->motion;
        s_cal_n++;
        if (--s_cal_left == 0) {
            float mean = s_cal_sum / s_cal_n;
            float var = s_cal_sum2 / s_cal_n - mean * mean;
            float sd = var > 0 ? sqrtf(var) : 0;
            float thr = mean + 4 * sd;
            if (thr < 1.5f * mean) thr = 1.5f * mean;
            s_thr = thr;
            s_cal = true;
            save_threshold();
            ESP_LOGI(TAG, "cal done n=%d mean=%.1f sd=%.1f thr=%.1f", s_cal_n, mean, sd, s_thr);
        }
    }

    bool above = s->n > 0 && s->motion > s_thr;
    s_recent = ((s_recent << 1) | above) & 0x7;
    if (above) s_last_above_us = now;
    int hits = __builtin_popcount(s_recent);

    if (!s_present && hits >= 2) {
        s_present = true;
        ESP_LOGI(TAG, "state present=1 motion=%.1f thr=%.1f after_s=%" PRId64, s->motion, s_thr,
                 (now - s_state_since_us) / 1000000);
        s_state_since_us = now;
    } else if (s_present && now - s_last_above_us > (int64_t)CONFIG_CSI_HOLD_S * 1000000) {
        s_present = false;
        ESP_LOGI(TAG, "state present=0 motion=%.1f thr=%.1f after_s=%" PRId64, s->motion, s_thr,
                 (now - s_state_since_us) / 1000000);
        s_state_since_us = now;
    }

    if (s_motion_lines && !csi_raw_dump()) {
        ESP_LOGI(TAG, "m t=%" PRId64 " motion=%.1f thr=%.1f present=%d fps=%" PRIu32
                 " rssi=%d nf=%d%s",
                 now / 1000000, s->motion, s_thr, s_present, s->n, s->rssi, s->nf,
                 s_cal_left > 0 ? " cal=running" : "");
    }
}

static void console_task(void *arg)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    uint8_t c;
    for (;;) {
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') continue;
        ESP_LOGI(TAG, "cmd '%c'", c);
        if (c == 'i') log_status();
        else if (c == 'r') esp_restart();
        else if (c == 'b') {
            s_cal_sum = s_cal_sum2 = 0;
            s_cal_n = 0;
            s_cal_left = CAL_SECONDS;
            ESP_LOGI(TAG, "cal start seconds=%d (keep the room empty and still)", CAL_SECONDS);
        } else if (c == 'c') {
            csi_set_raw_dump(!csi_raw_dump());
            ESP_LOGI(TAG, "raw_dump=%d", csi_raw_dump());
        } else if (c == 'm') {
            s_motion_lines = !s_motion_lines;
            ESP_LOGI(TAG, "motion_lines=%d", s_motion_lines);
        }
    }
}

void app_main(void)
{
    static const char *const own_tags[] = {"main", "csi", "wifi_sta", "xiao_board"};
    for (size_t i = 0; i < sizeof(own_tags) / sizeof(own_tags[0]); i++) {
        esp_log_level_set(own_tags[i], ESP_LOG_DEBUG);
    }

    log_boot();
    xTaskCreate(console_task, "console", 3072, NULL, 5, NULL);

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = wifi_sta_start(20000);
    ESP_LOGI(TAG, "selftest wifi=%s join_ms=%" PRId64 " rssi=%d", err == ESP_OK ? "ok" : "FAIL",
             (esp_timer_get_time() - t0) / 1000, wifi_sta_rssi());
    while (!wifi_sta_connected()) vTaskDelay(pdMS_TO_TICKS(1000));

    load_threshold(); // NVS is up once wifi_sta_start has run
    ESP_LOGI(TAG, "threshold thr=%.1f cal=%d hold_s=%d", s_thr, s_cal, CONFIG_CSI_HOLD_S);

    s_state_since_us = esp_timer_get_time();
    err = csi_start(on_second);
    ESP_LOGI(TAG, "selftest csi=%s err=%s", err == ESP_OK ? "ok" : "FAIL", esp_err_to_name(err));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        log_status();
    }
}
