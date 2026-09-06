idf_component_register(
    SRCS "src/espaperplay_diaglog.c"
    INCLUDE_DIRS "include"
    REQUIRES board
    PRIV_REQUIRES freertos esp_timer
)
