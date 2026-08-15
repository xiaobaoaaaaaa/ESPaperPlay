idf_component_register(
    SRCS "src/espaperplay_board.c"
         "src/espaperplay_display.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_spi
)
