idf_component_register(
    SRCS "src/espaperplay_power.c"
    INCLUDE_DIRS "include"
    REQUIRES board epd input wifi clock freertos esp_driver_gpio esp_driver_uart esp_timer
)
