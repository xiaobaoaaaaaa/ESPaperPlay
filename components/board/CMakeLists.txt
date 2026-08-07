idf_component_register(
    SRCS "src/espaperplay_board.c"
    INCLUDE_DIRS "include"
    # NOTE: esp_driver_gpio / esp_driver_spi / esp_driver_i2c will be added
    #       to PRIV_REQUIRES once the board bus initialization is implemented.
)
