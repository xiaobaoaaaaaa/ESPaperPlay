idf_component_register(
    SRCS "src/espaperplay_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES system esp_wifi esp_netif esp_event
)
