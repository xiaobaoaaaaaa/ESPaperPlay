idf_component_register(
    SRCS "src/espaperplay_netip.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls espressif__cjson esp_timer
)
