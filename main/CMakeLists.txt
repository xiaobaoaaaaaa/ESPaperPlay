file(GLOB EEZ_CPP_FILES "../ui/*.cpp")
file(GLOB EEZ_C_FILES "../ui/*.c")

idf_component_register(SRCS "main.c"
                    "src/network/wifi_ctrl.c"
                    "src/sntp/sntp.c"
                    "src/hal/button.c"
                    "src/hal/power_save.c"
                    "src/display/epaper_driver.c"
                    "src/touch/touch.c"
                    "src/touch/gpioX.c"
                    "src/touch/i2c_ctp_ftxx06_xx36.c"
                    "src/touch/i2c_config.c"
                    "src/lvgl/lvgl_init.c"
                    "../ui/ui_image_wifi_off.c"
                    "../ui/ui_image_wifi_connected_filled.c"
                    "../ui/ui_image_power_save.c"
                    
                    ${EEZ_CPP_FILES}
                    ${EEZ_C_FILES}

                INCLUDE_DIRS "."
                    "./include/network"
                    "./include/sntp"
                    "./include/hal"
                    "./include/display"
                    "./include/touch"
                    "./include/lvgl"

                PRIV_INCLUDE_DIRS "../ui")