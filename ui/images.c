#include "images.h"

const ext_img_desc_t images[14] = {
    { "wifi_off", &img_wifi_off },
    { "wifi_connected_low", &img_wifi_connected_low },
    { "wifi_connected_filled", &img_wifi_connected_filled },
    { "power_save", &img_power_save },
    { "icon_settings", &img_icon_settings },
    { "icon_clock", &img_icon_clock },
    { "icon_smart_home_manager", &img_icon_smart_home_manager },
    { "icon_weather", &img_icon_weather },
    { "weather_precip", &img_weather_precip },
    { "weather_humidity", &img_weather_humidity },
    { "weather_wind", &img_weather_wind },
    { "weather_visibility", &img_weather_visibility },
    { "weather_pressure", &img_weather_pressure },
    { "weather_dew_point", &img_weather_dew_point },
};
