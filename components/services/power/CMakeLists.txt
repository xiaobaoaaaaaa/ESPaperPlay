idf_component_register(
    SRCS "src/espaperplay_power.c"
    INCLUDE_DIRS "include"
    REQUIRES board epd input wifi clock diaglog ui freertos esp_driver_gpio esp_timer audio
    PRIV_REQUIRES esp_adc
)
