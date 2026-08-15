idf_component_register(
    SRCS "src/espaperplay_gui.c"
         "src/lvgl_port.c"
         "src/screens/screen_demo.c"
         "src/screens/screen_test.c"
         "src/screens/screen_benchmark.c"
         "src/screens/screen_home.c"
         "src/ui_stack.c"
    INCLUDE_DIRS "include"
    REQUIRES board epd touch lvgl
    PRIV_REQUIRES freertos heap esp_timer
)
