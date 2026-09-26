idf_component_register(
    SRCS "src/espaperplay_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES board diaglog
    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio freertos esp_timer
)
