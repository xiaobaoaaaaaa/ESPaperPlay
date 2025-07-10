#include "actions.h"
#include "ui.h"
#include "screens.h"
#include "esp_log.h"

void action_main_page_change_screen(lv_event_t *e) {
    // TODO: Implement action main_page_change_screen here
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_event_get_indev(e));
    if(event_code == LV_EVENT_GESTURE && dir == LV_DIR_LEFT) {
        ESP_LOGI("action_main_page_change_screen", "Gesture left detected, changing to next screen");
        eez_flow_set_screen(SCREEN_ID_MENU, LV_SCR_LOAD_ANIM_NONE, 0, 0);
    } else if(event_code == LV_EVENT_GESTURE && dir != LV_DIR_LEFT) {
        ESP_LOGI("action_main_page_change_screen", "Gesture not left, no screen change");
    } else if(event_code != LV_EVENT_GESTURE) {
        ESP_LOGI("action_main_page_change_screen", "Unknown event code: %d", event_code);
    }
}
