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
    FLOW_GLOBAL_VARIABLE_CURRENT_TIME = 0
};

// Native global variables

extern const char *get_var_current_weekday();
extern void set_var_current_weekday(const char *value);
extern bool get_var_wifi_connected();
extern void set_var_wifi_connected(bool value);
extern int32_t get_var_wifi_rssi();
extern void set_var_wifi_rssi(int32_t value);
extern bool get_var_is_power_save();
extern void set_var_is_power_save(bool value);


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_VARS_H*/