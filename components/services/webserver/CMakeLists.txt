idf_component_register(
    SRCS "src/espaperplay_webserver.c"
         "src/webserver_util.c"
         "src/webserver_handler_system.c"
         "src/webserver_handler_config.c"
         "src/webserver_handler_auth.c"
         "src/webserver_auth_guard.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "www/index.html"
    REQUIRES esp_http_server system wifi board auth session
    PRIV_REQUIRES espressif__cjson esp_timer esp_system
)
