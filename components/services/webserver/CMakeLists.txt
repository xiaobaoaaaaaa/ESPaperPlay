idf_component_register(
    SRCS "src/espaperplay_webserver.c"
         "src/webserver_util.c"
         "src/webserver_handler_system.c"
         "src/webserver_handler_config.c"
         "src/webserver_handler_weather.c"
         "src/webserver_handler_fonts.c"
         "src/webserver_handler_auth.c"
         "src/webserver_auth_guard.c"
         "src/webserver_tls.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "www/index.html"
    REQUIRES esp_http_server esp_https_server mbedtls nvs_flash esp_event esp_netif system wifi board auth session epd ui weather input storage fonts power
    PRIV_REQUIRES espressif__cjson esp_timer esp_system
)
