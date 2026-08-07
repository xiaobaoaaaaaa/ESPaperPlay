idf_component_register(
    SRCS "src/espaperplay_power.c"
    INCLUDE_DIRS "include"
    REQUIRES board
    # NOTE: esp_driver_gpio / esp_system (esp_sleep) go to PRIV_REQUIRES when
    #       the low-power implementation is added.
)
