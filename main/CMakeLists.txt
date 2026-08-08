idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board epd touch power storage system wifi ui reader input esp_timer
)
