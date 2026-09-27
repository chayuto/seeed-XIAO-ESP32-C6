idf_component_register(SRCS "coap_min.c" "philips_crypto.c" "philips_coap.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES mbedtls lwip esp_timer esp_netif freertos log)
