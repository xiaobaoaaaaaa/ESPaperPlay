#ifndef _SYS_CONFIG_H
#define _SYS_CONFIG_H

#include "stdbool.h"

typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    bool power_save_enabled;
    int power_save_min;
    char weather_city[32];
    char weather_api_key[64];
    char weather_api_host[64];
} system_config_t;

#endif