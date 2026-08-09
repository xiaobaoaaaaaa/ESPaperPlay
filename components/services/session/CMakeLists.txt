idf_component_register(
    SRCS "src/espaperplay_session.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls esp_hw_support esp_timer freertos
)
