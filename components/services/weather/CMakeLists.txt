idf_component_register(
    SRCS "src/espaperplay_weather.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls espressif__cjson esp_timer system wifi netip geoip espressif__zlib
)
