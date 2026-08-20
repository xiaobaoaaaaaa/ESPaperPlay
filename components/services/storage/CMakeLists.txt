idf_component_register(
    SRCS "src/espaperplay_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES board sd
    PRIV_REQUIRES fatfs sdmmc freertos
)