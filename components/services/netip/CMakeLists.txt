idf_component_register(
    SRCS "src/espaperplay_netip.c"
    INCLUDE_DIRS "include"
    REQUIRES nethttp espressif__cjson esp_timer
)
