idf_component_register(
    SRCS "src/espaperplay_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES board diaglog
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio freertos esp_timer
)
