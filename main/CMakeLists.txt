idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES auth board epd fonts touch power storage system session wifi webserver ui reader input nettime weather esp_timer
)
