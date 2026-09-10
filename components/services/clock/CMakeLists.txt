idf_component_register(
    SRCS "src/espaperplay_clock.c" "src/espaperplay_clock_lunar.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif nvs_flash nvs
)
