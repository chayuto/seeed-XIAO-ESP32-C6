#pragma once
// Station-mode Wi-Fi for the XIAO: powers the RF switch, joins CONFIG_XIAO_WIFI_SSID,
// reconnects on its own after a drop.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Initialise NVS, netif and Wi-Fi and start joining. Blocks up to timeout_ms for an IP;
// returns ESP_ERR_TIMEOUT if none arrived (the driver keeps trying in the background).
esp_err_t wifi_sta_start(uint32_t timeout_ms);

bool wifi_sta_connected(void);
int wifi_sta_rssi(void);             // dBm, or 0 when not associated
uint32_t wifi_sta_disconnects(void); // since boot
