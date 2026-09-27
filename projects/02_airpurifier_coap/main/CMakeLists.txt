idf_component_register(SRCS "main.c" "coap_min.c" "philips_crypto.c" "philips_coap.c"
                       INCLUDE_DIRS "."
                       REQUIRES xiao_board wifi_sta mbedtls json lwip esp_timer
                                esp_driver_usb_serial_jtag esp_app_format)
