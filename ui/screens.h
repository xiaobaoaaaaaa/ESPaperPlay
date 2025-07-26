#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *menu;
    lv_obj_t *settings;
    lv_obj_t *wifi_setings;
    lv_obj_t *obj0;
    lv_obj_t *obj0__state_time;
    lv_obj_t *obj0__state_wifi;
    lv_obj_t *obj0__power_save;
    lv_obj_t *obj1;
    lv_obj_t *obj1__state_time;
    lv_obj_t *obj1__state_wifi;
    lv_obj_t *obj1__power_save;
    lv_obj_t *img_settings;
    lv_obj_t *wlan_settings;
    lv_obj_t *switch_wlan;
    lv_obj_t *obj2;
    lv_obj_t *message_reconnect;
    lv_obj_t *current_time;
    lv_obj_t *current_weekday;
    lv_obj_t *current_date;
    lv_obj_t *obj3;
    lv_obj_t *obj4;
    lv_obj_t *state_wifi_main;
    lv_obj_t *power_save;
    lv_obj_t *yiyan;
    lv_obj_t *label_menu;
    lv_obj_t *label_settings;
    lv_obj_t *img_weather;
    lv_obj_t *label_weather;
    lv_obj_t *img_clock;
    lv_obj_t *label_clock;
    lv_obj_t *img_smart_home;
    lv_obj_t *label_smart_home;
    lv_obj_t *label_setings;
    lv_obj_t *ssid;
    lv_obj_t *obj5;
    lv_obj_t *rssi;
    lv_obj_t *obj6;
    lv_obj_t *ip;
    lv_obj_t *obj7;
    lv_obj_t *mac;
    lv_obj_t *obj8;
    lv_obj_t *primary;
    lv_obj_t *obj9;
    lv_obj_t *auth_mode;
    lv_obj_t *obj10;
    lv_obj_t *bandwidth;
    lv_obj_t *obj11;
} objects_t;

extern objects_t objects;

enum ScreensEnum {
    SCREEN_ID_MAIN = 1,
    SCREEN_ID_MENU = 2,
    SCREEN_ID_SETTINGS = 3,
    SCREEN_ID_WIFI_SETINGS = 4,
};

void create_screen_main();
void tick_screen_main();

void create_screen_menu();
void tick_screen_menu();

void create_screen_settings();
void tick_screen_settings();

void create_screen_wifi_setings();
void tick_screen_wifi_setings();

void create_user_widget_state_bar(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex);
void tick_user_widget_state_bar(void *flowState, int startWidgetIndex);

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/