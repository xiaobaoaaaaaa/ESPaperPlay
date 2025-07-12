#include <string>
#include "vars.h"

std::string current_weekday;

extern "C" const char *get_var_current_weekday() {
    return current_weekday.c_str();
}

extern "C" void set_var_current_weekday(const char *value) {
    current_weekday = value;
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