idf_component_register(
    SRCS "src/espaperplay_gui.c"
         "src/lvgl_port.c"
         "src/lvgl_touch.c"
         "src/screens/screen_boot.c"
         "src/screens/screen_home.c"
         "src/icons_data.c"
         "src/qweather_icons.c"
         "src/screens/screen_reader.c"
         "src/screens/screen_settings.c"
         "src/screens/screen_test.c"
         "src/screens/screen_weather.c"
         "src/ui_stack.c"
         "src/ui_key_input.c"
    INCLUDE_DIRS "include"
    REQUIRES board clock epd fonts touch lvgl input weather wifi system storage power
    PRIV_REQUIRES freertos heap esp_timer
)
