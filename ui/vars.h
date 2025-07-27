#ifndef EEZ_LVGL_UI_VARS_H
#define EEZ_LVGL_UI_VARS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// enum declarations



// Flow global variables

enum FlowGlobalVariables {
    FLOW_GLOBAL_VARIABLE_NONE
};

// Native global variables

extern const char *get_var_current_weekday();
extern void set_var_current_weekday(const char *value);
extern const char *get_var_current_time();
extern void set_var_current_time(const char *value);
extern bool get_var_is_power_save();
extern void set_var_is_power_save(bool value);
extern const char *get_var_yiyan();
extern void set_var_yiyan(const char *value);
extern bool get_var_ui_wifi_on_off();
extern void set_var_ui_wifi_on_off(bool value);
extern const char *get_var_current_date();
extern void set_var_current_date(const char *value);
extern bool get_var_wifi_connected();
extern void set_var_wifi_connected(bool value);
extern int32_t get_var_wifi_rssi();
extern void set_var_wifi_rssi(int32_t value);
extern const char *get_var_wifi_ssid();
extern void set_var_wifi_ssid(const char *value);
extern const char *get_var_wifi_ip();
extern void set_var_wifi_ip(const char *value);
extern const char *get_var_wifi_mac();
extern void set_var_wifi_mac(const char *value);
extern int32_t get_var_wifi_primary();
extern void set_var_wifi_primary(int32_t value);
extern const char *get_var_wifi_authmode();
extern void set_var_wifi_authmode(const char *value);
extern const char *get_var_wifi_bandwidth();
extern void set_var_wifi_bandwidth(const char *value);
extern bool get_var_is_power_save_enabled();
extern void set_var_is_power_save_enabled(bool value);
extern int32_t get_var_power_save_min();
extern void set_var_power_save_min(int32_t value);


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_VARS_H*/