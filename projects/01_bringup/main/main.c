// 01_bringup — XIAO ESP32-C6 board census.
// Prints what the chip says about itself, blinks the user LED so a person can confirm its
// polarity, reports BOOT presses, joins Wi-Fi through the RF switch and then heartbeats.
// Serial: 'i' = status now, 'r' = restart, 'p' = deliberate panic (tests the crash path:
// backtrace on the console, core dump in flash).
#include <inttypes.h>
#include <stdio.h>
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
#include "wifi_sta.h"
#include "xiao_board.h"

static const char *TAG = "main";

static uint32_t s_presses;

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
    ESP_LOGI(TAG, "chip model=%d rev=%d.%d cores=%d wifi=%d ble=%d 802154=%d flash_mb=%" PRIu32
             " mac=" MACSTR,
             chip.model, chip.revision / 100, chip.revision % 100, chip.cores,
             !!(chip.features & CHIP_FEATURE_WIFI_BGN), !!(chip.features & CHIP_FEATURE_BLE),
             !!(chip.features & CHIP_FEATURE_IEEE802154), flash_bytes / (1024 * 1024), MAC2STR(mac));
}

static void log_status(void)
{
    ESP_LOGI(TAG, "hb up=%" PRIu64 " heap=%" PRIu32 " heap_min=%" PRIu32
             " wifi=%d rssi=%d disconnects=%" PRIu32 " presses=%" PRIu32,
             esp_timer_get_time() / 1000000, esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size(), wifi_sta_connected(), wifi_sta_rssi(),
             wifi_sta_disconnects(), s_presses);
}

// Three slow blinks: LED on for 1 s, off for 1 s. If the LED is instead lit during the
// "off" phases, it is active-high and XIAO_PIN_LED's polarity is wrong.
static void led_selftest(void)
{
    ESP_LOGI(TAG, "selftest led: 3 x (on 1s, off 1s) starting now");
    for (int i = 0; i < 3; i++) {
        xiao_board_led_set(true);
        vTaskDelay(pdMS_TO_TICKS(1000));
        xiao_board_led_set(false);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "selftest led=done (needs a human to confirm)");
}

static void console_task(void *arg)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    uint8_t c;
    for (;;) {
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        ESP_LOGI(TAG, "cmd '%c'", c);
        if (c == 'i') log_status();
        else if (c == 'r') esp_restart();
        else if (c == 'p') {
            ESP_LOGW(TAG, "deliberate panic requested");
            vTaskDelay(pdMS_TO_TICKS(100));
            volatile int *bad = NULL;
            *bad = 42;
        }
    }
}

void app_main(void)
{
    static const char *const own_tags[] = {"main", "wifi_sta", "xiao_board"};
    for (size_t i = 0; i < sizeof(own_tags) / sizeof(own_tags[0]); i++) {
        esp_log_level_set(own_tags[i], ESP_LOG_DEBUG);
    }

    log_boot();
    ESP_ERROR_CHECK(xiao_board_led_init());
    ESP_ERROR_CHECK(xiao_board_button_init());
    xTaskCreate(console_task, "console", 3072, NULL, 5, NULL);

    led_selftest();

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = wifi_sta_start(20000);
    ESP_LOGI(TAG, "selftest wifi=%s join_ms=%" PRId64 " rssi=%d", err == ESP_OK ? "ok" : "FAIL",
             (esp_timer_get_time() - t0) / 1000, wifi_sta_rssi());

    bool was_pressed = false;
    int64_t last_hb = 0;
    for (;;) {
        bool pressed = xiao_board_button_pressed();
        if (pressed != was_pressed) {
            if (pressed) s_presses++;
            ESP_LOGI(TAG, "button %s count=%" PRIu32, pressed ? "down" : "up", s_presses);
            xiao_board_led_set(pressed);
            was_pressed = pressed;
        }
        int64_t now = esp_timer_get_time();
        if (now - last_hb >= 30LL * 1000000) {
            last_hb = now;
            log_status();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
