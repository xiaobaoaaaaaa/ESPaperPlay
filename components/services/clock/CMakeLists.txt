idf_component_register(
    SRCS "src/espaperplay_clock.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif nvs_flash
)
