idf_component_register(
    SRCS "src/espaperplay_hitokoto.c"
    INCLUDE_DIRS "include"
    REQUIRES nethttp espressif__cjson wifi nvs
    PRIV_REQUIRES freertos
)
