#ifndef _SYS_CONFIG_H
#define _SYS_CONFIG_H

#include "stdbool.h"

typedef struct {
    char wifi_ssid[32];             // WiFi 名称
    char wifi_password[64];         // WiFi 密码
    bool power_save_enabled;        // 是否启用省电模式
    int power_save_min;             // 省电模式等待时间，单位：分钟
    char weather_city[32];          // 天气城市
    char weather_api_key[64];       // 天气API密钥
    char weather_api_host[64];      // 天气API主机
    int max_partial_refresh_count;  // 最大局部刷新次数
} system_config_t;

#endif