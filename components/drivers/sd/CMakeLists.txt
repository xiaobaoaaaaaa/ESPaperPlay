idf_component_register(
    SRCS "src/espaperplay_sd.c"
    INCLUDE_DIRS "include"
    REQUIRES board sdmmc
    PRIV_REQUIRES esp_driver_sdmmc esp_driver_gpio freertos
)