#include <stdio.h>
#include<time.h>
#include "actions.h"
#include "vars.h"
#include "ui.h"
#include "screens.h"
#include "esp_log.h"

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
            eez_flow_set_screen(screen_id, anim_type, 400, 0);
        }
        else
        {
            ESP_LOGI("action_user_change_screen", "Unknown gesture direction %d, target direction: %d", dir, gesture_dir);
        }
    }
}

void action_get_current_week(lv_event_t *e) 
{
    time_t now;
    struct tm timeinfo;
    
    time(&now);
    localtime_r(&now, &timeinfo);

    const char *week_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    set_var_current_weekday(week_days[timeinfo.tm_wday]);
}

