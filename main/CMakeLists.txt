idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES auth board epd touch power storage system session wifi ui reader input esp_timer
)
