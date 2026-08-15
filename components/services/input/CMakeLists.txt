idf_component_register(
    SRCS "src/espaperplay_input.c"
    INCLUDE_DIRS "include"
    REQUIRES touch board espressif__button freertos
    PRIV_REQUIRES esp_timer
)
