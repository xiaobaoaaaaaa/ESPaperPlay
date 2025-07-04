idf_component_register(SRCS "main.c"
                    "src/network/wifi_ctrl.c"
                    "src/sntp/sntp.c"
                    "src/hal/button.c"
                    INCLUDE_DIRS "."
                    "./include/network"
                    "./include/sntp"
                    "./include/hal")