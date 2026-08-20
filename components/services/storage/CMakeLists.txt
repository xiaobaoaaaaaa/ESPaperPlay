idf_component_register(
    SRCS "src/espaperplay_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES board
    # NOTE: fatfs / sdmmc / esp_vfs_fat / esp_driver_sdspi go to PRIV_REQUIRES
    #       when the SD card implementation is added.
)
