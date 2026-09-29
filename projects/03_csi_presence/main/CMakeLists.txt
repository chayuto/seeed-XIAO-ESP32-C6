idf_component_register(SRCS "main.c" "csi.c"
                       INCLUDE_DIRS "."
                       REQUIRES xiao_board wifi_sta esp_wifi esp_netif lwip nvs_flash
                                esp_driver_usb_serial_jtag spi_flash esp_timer esp_app_format)
