idf_component_register(
    SRCS "src/espaperplay_gui.c"
         "src/espaperplay_lv_mem_psram.c"
         "src/lvgl_port.c"
         "src/lvgl_touch.c"
         "src/screens/screen_boot.c"
         "src/screens/screen_home.c"
         "src/icons_data.c"
         "src/qweather_icons.c"
         "src/screens/screen_reader.c"
         "src/screens/screen_reader_home.c"
         "src/screens/screen_settings.c"
         "src/screens/screen_files.c"
         "src/screens/screen_setup.c"
         "src/screens/screen_wifi_list.c"
         "src/screens/screen_test.c"
         "src/screens/screen_weather.c"
         "src/ui_stack.c"
         "src/ui_key_input.c"
    INCLUDE_DIRS "include"
    REQUIRES board clock epd fonts touch lvgl input weather wifi system storage power reader
    PRIV_REQUIRES freertos heap esp_timer
    # lv_malloc_core 等（espaperplay_lv_mem_psram.c）被 liblvgl 的 lv_mem.c 后置
    # 引用：链接单遍扫描会错过 libui 归档成员，须整库编入保证可见
    WHOLE_ARCHIVE
)
