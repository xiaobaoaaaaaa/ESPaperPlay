idf_component_register(
    SRCS "src/espaperplay_epd.c"
    INCLUDE_DIRS "include"
    REQUIRES board
    # NOTE: esp_driver_spi / esp_driver_gpio go to PRIV_REQUIRES when the
    #       UC8179 driver is implemented.
)
