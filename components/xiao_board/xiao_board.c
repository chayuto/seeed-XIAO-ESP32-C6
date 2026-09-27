#include "xiao_board.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "xiao_board";

esp_err_t xiao_board_rf_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << XIAO_PIN_RF_ENABLE) | (1ULL << XIAO_PIN_RF_ANT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "rf gpio");
    gpio_set_level(XIAO_PIN_RF_ENABLE, 0);
#if CONFIG_XIAO_ANTENNA_EXTERNAL
    gpio_set_level(XIAO_PIN_RF_ANT, 1);
    ESP_LOGI(TAG, "rf switch=on antenna=external");
#else
    gpio_set_level(XIAO_PIN_RF_ANT, 0);
    ESP_LOGI(TAG, "rf switch=on antenna=ceramic");
#endif
    return ESP_OK;
}

esp_err_t xiao_board_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << XIAO_PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "led gpio");
    xiao_board_led_set(false);
    return ESP_OK;
}

void xiao_board_led_set(bool on)
{
    gpio_set_level(XIAO_PIN_LED, on ? 0 : 1);
}

esp_err_t xiao_board_button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << XIAO_PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&io);
}

bool xiao_board_button_pressed(void)
{
    return gpio_get_level(XIAO_PIN_BOOT) == 0;
}
