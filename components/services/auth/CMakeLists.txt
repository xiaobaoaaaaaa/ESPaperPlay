idf_component_register(
    SRCS "src/espaperplay_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls esp_hw_support nvs_flash
)
