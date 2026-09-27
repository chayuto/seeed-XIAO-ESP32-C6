idf_component_register(SRCS "main.c" "bucket.c" "uploader.c"
                       INCLUDE_DIRS "."
                       REQUIRES xiao_board wifi_sta philips_air cloud_upload json esp_timer
                                esp_driver_usb_serial_jtag esp_app_format nvs_flash)
