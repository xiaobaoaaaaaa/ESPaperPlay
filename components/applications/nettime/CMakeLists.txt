idf_component_register(
    SRCS "src/espaperplay_nettime.c"
    INCLUDE_DIRS "include"
    REQUIRES wifi netip geoip clock
)
