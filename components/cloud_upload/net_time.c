#include "net_time.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

static const char *TAG = "net_time";

// epoch_us - uptime_us. 64-bit on a 32-bit core, so it's written in a critical section:
// ids are minted from it, and a torn read would bake a wrong timestamp into a row forever.
static int64_t s_offset_us;
static volatile bool s_synced;
static int64_t s_last_sync_us = -1;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// A clock can't legitimately predate the firmware reading it. __DATE__ is the build
// machine's local date, so back off two days to stay clear of time-zone skew.
static int64_t build_floor_s(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 0, year = 0;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) return 0;
    const char *p = strstr(months, mon);
    if (!p) return 0;
    struct tm t = {.tm_mon = (int)((p - months) / 3), .tm_mday = day, .tm_year = year - 1900};
    time_t e = mktime(&t);
    return e == (time_t)-1 ? 0 : (int64_t)e - 2 * 86400;
}

static void on_sync(struct timeval *tv)
{
    int64_t epoch_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
    if (tv->tv_sec < build_floor_s()) {
        ESP_LOGW(TAG, "sntp time %lld predates the build - ignored", (long long)tv->tv_sec);
        return;
    }
    int64_t now = esp_timer_get_time();
    int64_t prev = s_offset_us;
    portENTER_CRITICAL(&s_mux);
    s_offset_us = epoch_us - now;
    s_last_sync_us = now;
    portEXIT_CRITICAL(&s_mux);
    bool first = !s_synced;
    s_synced = true;
    struct tm utc;
    time_t secs = tv->tv_sec;
    gmtime_r(&secs, &utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    ESP_LOGI(TAG, "sntp synced utc=%s first=%d step_ms=%lld", buf, first,
             first ? 0LL : (long long)((s_offset_us - prev) / 1000));
}

void net_time_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_XIAO_SNTP_SERVER);
    cfg.sync_cb = on_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "sntp start server=%s err=%s", CONFIG_XIAO_SNTP_SERVER, esp_err_to_name(err));
}

bool net_time_synced(void)
{
    return s_synced;
}

int64_t net_time_epoch_ms_at(int64_t uptime_us)
{
    if (!s_synced) return -1;
    portENTER_CRITICAL(&s_mux);
    int64_t off = s_offset_us;
    portEXIT_CRITICAL(&s_mux);
    return (uptime_us + off) / 1000;
}

int64_t net_time_epoch_ms_now(void)
{
    return net_time_epoch_ms_at(esp_timer_get_time());
}

int64_t net_time_since_sync_s(void)
{
    if (!s_synced) return -1;
    return (esp_timer_get_time() - s_last_sync_us) / 1000000;
}
