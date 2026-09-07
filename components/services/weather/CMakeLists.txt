idf_component_register(
    SRCS "src/espaperplay_weather.c"
    INCLUDE_DIRS "include"
    REQUIRES nethttp espressif__cjson esp_timer system wifi netip geoip espressif__zlib
)
