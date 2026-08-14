idf_component_register(
    SRCS "src/espaperplay_gui.c"
    INCLUDE_DIRS "include"
    REQUIRES board epd touch
    PRIV_REQUIRES freertos heap esp_timer
)
