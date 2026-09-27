#pragma once
// Board support for the Seeed XIAO ESP32-C6: RF switch, user LED, BOOT button.
#include <stdbool.h>
#include "esp_err.h"

#define XIAO_PIN_RF_ENABLE 3   // drive LOW to power the RF switch
#define XIAO_PIN_RF_ANT    14  // LOW = on-board ceramic, HIGH = u.FL
#define XIAO_PIN_LED       15  // user LED (yellow), active-low
#define XIAO_PIN_BOOT      9   // BOOT button, pull-up, LOW when pressed

// Power the RF switch and select the antenna from CONFIG_XIAO_ANTENNA_EXTERNAL.
// Call before esp_wifi_start().
esp_err_t xiao_board_rf_init(void);

esp_err_t xiao_board_led_init(void);
void xiao_board_led_set(bool on);

esp_err_t xiao_board_button_init(void);
bool xiao_board_button_pressed(void);
