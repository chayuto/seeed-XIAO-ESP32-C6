idf_component_register(SRCS "uuid7.c" "net_time.c" "supabase.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_netif esp_timer log)
