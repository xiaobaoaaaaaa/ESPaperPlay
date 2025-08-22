#include <string>
#include "vars.h"

std::string current_weekday;

extern "C" const char *get_var_current_weekday() {
    return current_weekday.c_str();
}

extern "C" void set_var_current_weekday(const char *value) {
    current_weekday = value;
}

std::string current_time;

extern "C" const char *get_var_current_time() {
    return current_time.c_str();
}

extern "C" void set_var_current_time(const char *value) {
    current_time = value;
}

std::string current_date;

extern "C" const char *get_var_current_date() {
    return current_date.c_str();
}

extern "C" void set_var_current_date(const char *value) {
    current_date = value;
}

bool wifi_connected;

extern "C" bool get_var_wifi_connected() {
    return wifi_connected;
}

extern "C" void set_var_wifi_connected(bool value) {
    wifi_connected = value;
}

int32_t wifi_rssi;

extern "C" int32_t get_var_wifi_rssi() {
    return wifi_rssi;
}

extern "C" void set_var_wifi_rssi(int32_t value) {
    wifi_rssi = value;
}

bool is_power_save;

bool get_var_is_power_save() {
    return is_power_save;
}

void set_var_is_power_save(bool value) {
    is_power_save = value;
}

std::string yiyan;

extern "C" const char *get_var_yiyan() {
    return yiyan.c_str();
}

extern "C" void set_var_yiyan(const char *value) {
    yiyan = value;
}

bool ui_wifi_on_off;

extern "C" bool get_var_ui_wifi_on_off() {
    return ui_wifi_on_off;
}

extern "C" void set_var_ui_wifi_on_off(bool value) {
    ui_wifi_on_off = value;
}

std::string wifi_ssid;

extern "C" const char *get_var_wifi_ssid() {
    return wifi_ssid.c_str();
}

extern "C" void set_var_wifi_ssid(const char *value) {
    wifi_ssid = value;
}

std::string wifi_ip;

extern "C" const char *get_var_wifi_ip() {
    return wifi_ip.c_str();
}

extern "C" void set_var_wifi_ip(const char *value) {
    wifi_ip = value;
}

std::string wifi_mac;

extern "C" const char *get_var_wifi_mac() {
    return wifi_mac.c_str();
}

extern "C" void set_var_wifi_mac(const char *value) {
    wifi_mac = value;
}

int32_t wifi_primary;

extern "C" int32_t get_var_wifi_primary() {
    return wifi_primary;
}

extern "C" void set_var_wifi_primary(int32_t value) {
    wifi_primary = value;
}

std::string wifi_authmode;

extern "C" const char *get_var_wifi_authmode() {
    return wifi_authmode.c_str();
}

extern "C" void set_var_wifi_authmode(const char *value) {
    wifi_authmode = value;
}

std::string wifi_bandwidth;

extern "C" const char *get_var_wifi_bandwidth() {
    return wifi_bandwidth.c_str();
}

extern "C" void set_var_wifi_bandwidth(const char *value) {
    wifi_bandwidth = value;
}

bool is_power_save_enabled;

extern "C" bool get_var_is_power_save_enabled() {
    return is_power_save_enabled;
}

extern "C" void set_var_is_power_save_enabled(bool value) {
    is_power_save_enabled = value;
}

int32_t power_save_min;

extern "C" int32_t get_var_power_save_min() {
    return power_save_min;
}

extern "C" void set_var_power_save_min(int32_t value) {
    power_save_min = value;
}

std::string weather_city;

extern "C" const char *get_var_weather_city() {
    return weather_city.c_str();
}

extern "C" void set_var_weather_city(const char *value) {
    weather_city = value;
}

std::string weather;

extern "C" const char *get_var_weather() {
    return weather.c_str();
}

extern "C" void set_var_weather(const char *value) {
    weather = value;
}

float weather_temp;

extern "C" float get_var_weather_temp() {
    return weather_temp;
}

extern "C" void set_var_weather_temp(float value) {
    weather_temp = value;
}

float weather_feels_like;

extern "C" float get_var_weather_feels_like() {
    return weather_feels_like;
}

extern "C" void set_var_weather_feels_like(float value) {
    weather_feels_like = value;
}

float weather_humidity;

extern "C" float get_var_weather_humidity() {
    return weather_humidity;
}

extern "C" void set_var_weather_humidity(float value) {
    weather_humidity = value;
}

std::string weather_wind_dir;

extern "C" const char *get_var_weather_wind_dir() {
    return weather_wind_dir.c_str();
}

extern "C" void set_var_weather_wind_dir(const char *value) {
    weather_wind_dir = value;
}

std::string weather_wind_scale;

extern "C" const char *get_var_weather_wind_scale() {
    return weather_wind_scale.c_str();
}

extern "C" void set_var_weather_wind_scale(const char *value) {
    weather_wind_scale = value;
}

float weather_wind_speed;

extern "C" float get_var_weather_wind_speed() {
    return weather_wind_speed;
}

extern "C" void set_var_weather_wind_speed(float value) {
    weather_wind_speed = value;
}

float weather_precip;

extern "C" float get_var_weather_precip() {
    return weather_precip;
}

extern "C" void set_var_weather_precip(float value) {
    weather_precip = value;
}

float weather_pressure;

extern "C" float get_var_weather_pressure() {
    return weather_pressure;
}

extern "C" void set_var_weather_pressure(float value) {
    weather_pressure = value;
}

float weather_visibility;

extern "C" float get_var_weather_visibility() {
    return weather_visibility;
}

extern "C" void set_var_weather_visibility(float value) {
    weather_visibility = value;
}

float weather_cloud;

extern "C" float get_var_weather_cloud() {
    return weather_cloud;
}

extern "C" void set_var_weather_cloud(float value) {
    weather_cloud = value;
}

float weather_dew_point;

extern "C" float get_var_weather_dew_point() {
    return weather_dew_point;
}

extern "C" void set_var_weather_dew_point(float value) {
    weather_dew_point = value;
}

std::string weather_update_time;

extern "C" const char *get_var_weather_update_time() {
    return weather_update_time.c_str();
}

extern "C" void set_var_weather_update_time(const char *value) {
    weather_update_time = value;
}

bool weather_updated;

extern "C" bool get_var_weather_updated() {
    return weather_updated;
}

extern "C" void set_var_weather_updated(bool value) {
    weather_updated = value;
}

std::string temp_max;

extern "C" const char *get_var_temp_max() {
    return temp_max.c_str();
}

extern "C" void set_var_temp_max(const char *value) {
    temp_max = value;
}

std::string temp_min;

extern "C" const char *get_var_temp_min() {
    return temp_min.c_str();
}

extern "C" void set_var_temp_min(const char *value) {
    temp_min = value;
}

std::string weather_day;

extern "C" const char *get_var_weather_day() {
    return weather_day.c_str();
}

extern "C" void set_var_weather_day(const char *value) {
    weather_day = value;
}

std::string weather_night;

extern "C" const char *get_var_weather_night() {
    return weather_night.c_str();
}

extern "C" void set_var_weather_night(const char *value) {
    weather_night = value;
}

std::string tcp_msg;

extern "C" const char *get_var_tcp_msg() {
    return tcp_msg.c_str();
}

extern "C" void set_var_tcp_msg(const char *value) {
    tcp_msg = value;
}

std::string weather_city_in_nvs;

extern "C" const char *get_var_weather_city_in_nvs() {
    return weather_city_in_nvs.c_str();
}

extern "C" void set_var_weather_city_in_nvs(const char *value) {
    weather_city_in_nvs = value;
}

std::string weather_api_key;

extern "C" const char *get_var_weather_api_key() {
    return weather_api_key.c_str();
}

extern "C" void set_var_weather_api_key(const char *value) {
    weather_api_key = value;
}

std::string weather_api_host;

extern "C" const char *get_var_weather_api_host() {
    return weather_api_host.c_str();
}

extern "C" void set_var_weather_api_host(const char *value) {
    weather_api_host = value;
}

std::string weather_icon;

extern "C" const char *get_var_weather_icon() {
    return weather_icon.c_str();
}

extern "C" void set_var_weather_icon(const char *value) {
    weather_icon = value;
}
 
bool is_timer_started;

extern "C" bool get_var_is_timer_started() {
    return is_timer_started;
}

extern "C" void set_var_is_timer_started(bool value) {
    is_timer_started = value;
}

bool timer_paused;

extern "C" bool get_var_timer_paused() {
    return timer_paused;
}

extern "C" void set_var_timer_paused(bool value) {
    timer_paused = value;
}
