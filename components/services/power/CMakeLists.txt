idf_component_register(
    SRCS "src/espaperplay_power.c"
    INCLUDE_DIRS "include"
    REQUIRES board epd input wifi clock weather diaglog ui freertos esp_driver_gpio esp_driver_uart esp_timer
)
