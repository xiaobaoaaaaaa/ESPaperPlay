#ifndef EEZ_LVGL_UI_EVENTS_H
#define EEZ_LVGL_UI_EVENTS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void action_user_change_screen(lv_event_t * e);
extern void action_check_wifi_status(lv_event_t * e);
extern void action_get_yiyan(lv_event_t * e);
extern void action_set_wifi_on_off(lv_event_t * e);
extern void action_get_wifi_ap_info(lv_event_t * e);
extern void action_wifi_reconnect(lv_event_t * e);
extern void action_power_save_on_off(lv_event_t * e);
extern void action_check_power_save_mode(lv_event_t * e);
extern void action_set_power_save_min(lv_event_t * e);
extern void action_get_weather(lv_event_t * e);
extern void action_get_weather_settings(lv_event_t * e);
extern void action_get_tcp_msg(lv_event_t * e);
extern void action_save_weather_config(lv_event_t * e);
extern void action_close_tcp_server(lv_event_t * e);


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_EVENTS_H*/