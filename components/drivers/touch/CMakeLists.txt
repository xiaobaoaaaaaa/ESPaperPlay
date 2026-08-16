idf_component_register(
    SRCS "src/espaperplay_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES board
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio freertos
)
