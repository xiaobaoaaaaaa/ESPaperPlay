idf_component_register(
    SRCS "src/espaperplay_epd.c"
    INCLUDE_DIRS "include"
    REQUIRES board
    PRIV_REQUIRES esp_driver_spi esp_driver_gpio freertos esp_timer
)
