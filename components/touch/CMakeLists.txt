idf_component_register(
    SRCS "src/espaperplay_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES board
    # NOTE: esp_driver_i2c / esp_driver_gpio go to PRIV_REQUIRES when the
    #       GT911 driver is implemented.
)
