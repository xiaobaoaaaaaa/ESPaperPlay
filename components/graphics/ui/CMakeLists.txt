idf_component_register(
    SRCS "src/espaperplay_gui.c"
         "src/lvgl_port.c"
         "src/lvgl_touch.c"
         "src/screens/screen_home.c"
         "src/screens/screen_test.c"
         "src/ui_stack.c"
         "src/ui_key_input.c"
    INCLUDE_DIRS "include"
    REQUIRES board clock epd touch lvgl input
    PRIV_REQUIRES freertos heap esp_timer
)
