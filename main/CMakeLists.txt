idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board epd touch power storage system ui reader input esp_timer
)
