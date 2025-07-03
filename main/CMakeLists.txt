idf_component_register(SRCS "main.c"
                    "src/network/wifi_ctrl.c"
                    "src/sntp/sntp.c"
                    INCLUDE_DIRS "."
                    "./include/network"
                    "./include/sntp")