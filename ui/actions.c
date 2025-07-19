#include <stdio.h>
#include<time.h>
#include "actions.h"
#include "vars.h"
#include "ui.h"
#include "screens.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "yiyan.h"
#include "wifi_ctrl.h"

void action_user_change_screen(lv_event_t *e) 
{
    lv_event_code_t event = lv_event_get_code(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_event_get_indev(e));
    int gesture_dir = (int)(uintptr_t)lv_event_get_user_data(e) / 10;
    int screen_id = (int)(uintptr_t)lv_event_get_user_data(e) % 10;
    if(event == LV_EVENT_GESTURE)
    { 
        if(dir == gesture_dir)
        {
            ESP_LOGI("action_user_change_screen", "Gesture direction %d detected, changing to  screen：%d", gesture_dir, screen_id);
            int anim_type;
            switch (gesture_dir)
            {
            case LV_DIR_LEFT:
                anim_type = LV_SCR_LOAD_ANIM_MOVE_LEFT;
                break;

            case LV_DIR_RIGHT:
                anim_type = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
                break;

            case LV_DIR_TOP:
                anim_type = LV_SCR_LOAD_ANIM_MOVE_TOP;
                break;

            case LV_DIR_BOTTOM:
                anim_type = LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
                break;
            
            default:
                anim_type = LV_SCR_LOAD_ANIM_MOVE_LEFT;
                break;
            }
            eez_flow_set_screen(screen_id, anim_type, 200, 0);
        }
        else
        {
            ESP_LOGI("action_user_change_screen", "Unknown gesture direction %d, target direction: %d", dir, gesture_dir);
        }
    }
}

int week_day = -1;
const char *week_days[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
void action_get_current_week(lv_event_t *e) 
{
    time_t now;
    struct tm timeinfo;
    if(week_day == -1)
    {
    
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year < 2025-1900) {
            ESP_LOGE("action_get_current_week", "Year is before 2016, cannot determine current weekday");
            return;
        }

        week_day = timeinfo.tm_wday;
        set_var_current_weekday(week_days[timeinfo.tm_wday]);
    }
    else
        set_var_current_weekday(week_days[++week_day % 7]);
}

bool wifi_signal_strength_check = false;

void action_check_wifi_status(lv_event_t *e) 
{
    set_var_ui_wifi_on_off(wifi_on_off);
    ESP_LOGI("action_check_wifi_status", "ui_wifi status: %d", get_var_ui_wifi_on_off());
    if(!wifi_on_off) return;

    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

    if (ret == ESP_OK) 
    {
        int rssi = ap_info.rssi;
        if(rssi > -60 && !wifi_signal_strength_check)
        {
            set_var_wifi_rssi(rssi);
            wifi_signal_strength_check = true;
        }
        else if(rssi <= -60 && wifi_signal_strength_check)
        {
            set_var_wifi_rssi(rssi);
            wifi_signal_strength_check = false;
        }
        set_var_wifi_connected(true);
    } 
    else 
    {
        set_var_wifi_connected(false);
    }
}

void action_get_yiyan(lv_event_t *e) 
{
    get_yiyan();
}

void action_set_wifi_on_off(lv_event_t *e) 
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on_off = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if(on_off)
    {
        if(wifi_on_off)
            return;
        else
        {
            ESP_LOGI("action_set_wifi_on_off", "Set WiFi on");
            esp_wifi_start();
            esp_wifi_connect();
            set_wifi_on_off(true);
        }
    }
    else
    {
        if(!wifi_on_off) 
            return;
        else
        {
            ESP_LOGI("action_set_wifi_on_off", "Set WiFi off");
            esp_wifi_disconnect();
            esp_wifi_stop();
            set_wifi_on_off(false);
        }
    }
}
