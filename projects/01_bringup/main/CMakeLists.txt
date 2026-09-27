idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES xiao_board wifi_sta esp_driver_usb_serial_jtag spi_flash esp_timer esp_app_format)
