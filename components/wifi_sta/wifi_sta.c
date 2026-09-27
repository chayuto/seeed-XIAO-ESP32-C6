#include "wifi_sta.h"
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "xiao_board.h"

static const char *TAG = "wifi_sta";

#define GOT_IP_BIT BIT0

static EventGroupHandle_t s_events;
static volatile bool s_connected;
static volatile uint32_t s_disconnects;
static volatile bool s_suspended;
static esp_timer_handle_t s_resume_timer;

static void resume_cb(void *arg)
{
    s_suspended = false;
    ESP_LOGW(TAG, "suspend over - rejoining");
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_connected = false;
        s_disconnects++;
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        ESP_LOGW(TAG, "disconnected reason=%d count=%lu suspended=%d", d->reason,
                 (unsigned long)s_disconnects, s_suspended);
        if (!s_suspended) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_connected = true;
        xEventGroupSetBits(s_events, GOT_IP_BIT);
        ESP_LOGI(TAG, "got_ip ip=" IPSTR " gw=" IPSTR " rssi=%d",
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw), wifi_sta_rssi());
    }
}

esp_err_t wifi_sta_start(uint32_t timeout_ms)
{
    if (strlen(CONFIG_XIAO_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "no SSID: set CONFIG_XIAO_WIFI_SSID in sdkconfig.defaults.local");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(xiao_board_rf_init());

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_XIAO_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_XIAO_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep delays inbound UDP (CoAP notifications) by up to a DTIM interval; the
    // S3 sibling also needed it off to stay reachable. Revisit for battery projects.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "joining ssid_len=%u", (unsigned)strlen(CONFIG_XIAO_WIFI_SSID));

    EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifi_sta_connected(void)
{
    return s_connected;
}

int wifi_sta_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

uint32_t wifi_sta_disconnects(void)
{
    return s_disconnects;
}

void wifi_sta_suspend(uint32_t seconds)
{
    if (!s_resume_timer) {
        const esp_timer_create_args_t a = {.callback = resume_cb, .name = "wifi_resume"};
        ESP_ERROR_CHECK(esp_timer_create(&a, &s_resume_timer));
    }
    esp_timer_stop(s_resume_timer);
    s_suspended = true;
    ESP_LOGW(TAG, "suspend for %lu s (test)", (unsigned long)seconds);
    esp_wifi_disconnect();
    esp_timer_start_once(s_resume_timer, (uint64_t)seconds * 1000000);
}
