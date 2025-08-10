file(GLOB EEZ_CPP_FILES "../ui/*.cpp")
file(GLOB EEZ_C_FILES "../ui/*.c")

idf_component_register(SRCS "main.c"
                    "src/network/wifi_ctrl.c"
                    "src/network/tcpserver.c"
                    "src/sntp/sntp.c"
                    "src/sntp/date_update.c"
                    "src/hal/button.c"
                    "src/hal/power_save.c"
                    "src/hal/config_manager.c"
                    "src/display/epaper_driver.c"
                    "src/touch/touch.c"
                    "src/touch/gpioX.c"
                    "src/touch/i2c_ctp_ftxx06_xx36.c"
                    "src/touch/i2c_config.c"
                    "src/lvgl/lvgl_init.c"
                    "src/apps/weather.c"
                    "src/apps/yiyan.c"
                    "../ui/ui_image_wifi_off.c"
                    "../ui/ui_image_wifi_connected_filled.c"
                    "../ui/ui_image_power_save.c"
                    "../ui/ui_image_icon_clock.c"
                    "../ui/ui_image_icon_settings.c"
                    "../ui/ui_image_icon_smart_home_manager.c"
                    "../ui/ui_image_icon_weather.c"
                    "../ui/ui_image_weather_dew_point.c"
                    "../ui/ui_image_weather_humidity.c"
                    "../ui/ui_image_weather_precip.c"
                    "../ui/ui_image_weather_pressure.c"
                    "../ui/ui_image_weather_visibility.c"
                    "../ui/ui_image_weather_wind.c"

                    ${EEZ_CPP_FILES}
                    ${EEZ_C_FILES}

                INCLUDE_DIRS "."
                    "./include/network"
                    "./include/sntp"
                    "./include/hal"
                    "./include/display"
                    "./include/touch"
                    "./include/lvgl"
                    "./include/apps"

                PRIV_INCLUDE_DIRS "../ui")