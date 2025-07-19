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