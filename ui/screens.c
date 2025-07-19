#include <string.h>

#include "screens.h"
#include "images.h"
#include "fonts.h"
#include "actions.h"
#include "vars.h"
#include "styles.h"
#include "ui.h"

#include <string.h>

objects_t objects;
lv_obj_t *tick_value_change_obj;

static void event_handler_cb_main_main(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    void *flowState = lv_event_get_user_data(e);
    (void)flowState;
    
    if (event == LV_EVENT_GESTURE) {
        e->user_data = (void *)12;
        action_user_change_screen(e);
    }
}

static void event_handler_cb_menu_menu(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    void *flowState = lv_event_get_user_data(e);
    (void)flowState;
    
    if (event == LV_EVENT_GESTURE) {
        e->user_data = (void *)21;
        action_user_change_screen(e);
    }
}

static void event_handler_cb_menu_img_settings(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    void *flowState = lv_event_get_user_data(e);
    (void)flowState;
    
    if (event == LV_EVENT_CLICKED) {
        e->user_data = (void *)0;
        flowPropagateValueLVGLEvent(flowState, 3, 0, e);
    }
}

static void event_handler_cb_settings_settings(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    void *flowState = lv_event_get_user_data(e);
    (void)flowState;
    
    if (event == LV_EVENT_GESTURE) {
        e->user_data = (void *)22;
        action_user_change_screen(e);
    }
}

static void event_handler_cb_settings_switch_wlan(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    void *flowState = lv_event_get_user_data(e);
    (void)flowState;
    
    if (event == LV_EVENT_CLICKED) {
        e->user_data = (void *)0;
        flowPropagateValueLVGLEvent(flowState, 5, 0, e);
    }
}

void create_screen_main() {
    void *flowState = getFlowState(0, 0);
    (void)flowState;
    lv_obj_t *obj = lv_obj_create(0);
    objects.main = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 200, 200);
    lv_obj_add_event_cb(obj, event_handler_cb_main_main, LV_EVENT_ALL, flowState);
    {
        lv_obj_t *parent_obj = obj;
        {
            // current_time
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.current_time = obj;
            lv_obj_set_pos(obj, 0, -53);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "00:00");
        }
        {
            // current_weekday
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.current_weekday = obj;
            lv_obj_set_pos(obj, 118, 66);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &ui_font_siyuanheiti_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "星期日");
        }
        {
            // current_date
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.current_date = obj;
            lv_obj_set_pos(obj, -23, -21);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &ui_font_siyuanheiti_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "2025-01-01");
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            objects.obj2 = obj;
            lv_obj_set_pos(obj, -2, 93);
            lv_obj_set_size(obj, 45, 26);
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0xff000000), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.obj3 = obj;
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xffffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_font(obj, &ui_font_siyuanheiti_14, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "一言");
                }
            }
        }
        {
            // state_wifi_main
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.state_wifi_main = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_wifi_off);
            lv_obj_set_style_align(obj, LV_ALIGN_TOP_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            // power_save
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.power_save = obj;
            lv_obj_set_pos(obj, 172, 0);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_power_save);
        }
        {
            // yiyan
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.yiyan = obj;
            lv_obj_set_pos(obj, 0, 119);
            lv_obj_set_size(obj, 200, 81);
            lv_obj_set_style_text_font(obj, &ui_font_siyuanheiti_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_line_space(obj, -5, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "你好，\n世界！");
        }
    }
    
    tick_screen_main();
}

void tick_screen_main() {
    void *flowState = getFlowState(0, 0);
    (void)flowState;
}

void create_screen_menu() {
    void *flowState = getFlowState(0, 1);
    (void)flowState;
    lv_obj_t *obj = lv_obj_create(0);
    objects.menu = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 200, 200);
    lv_obj_add_event_cb(obj, event_handler_cb_menu_menu, LV_EVENT_ALL, flowState);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            objects.obj0 = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 200, 20);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            create_user_widget_state_bar(obj, getFlowState(flowState, 0), 4);
        }
        {
            // label_menu
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.label_menu = obj;
            lv_obj_set_pos(obj, 0, -4);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_align(obj, LV_ALIGN_TOP_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &ui_font_siyuanheiti_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "菜单");
        }
        {
            // img_settings
            lv_obj_t *obj = lv_imagebutton_create(parent_obj);
            objects.img_settings = obj;
            lv_obj_set_pos(obj, 15, 22);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, 70);
            lv_imagebutton_set_src(obj, LV_IMAGEBUTTON_STATE_RELEASED, NULL, &img_icon_settings, NULL);
            lv_imagebutton_set_src(obj, LV_IMAGEBUTTON_STATE_PRESSED, NULL, &img_icon_settings, NULL);
            lv_obj_add_event_cb(obj, event_handler_cb_menu_img_settings, LV_EVENT_ALL, flowState);
        }
        {
            // label_settings
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.label_settings = obj;
            lv_obj_set_pos(obj, 36, 85);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &ui_font_siyuanheiti_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "设置");
        }
        {
            // img_weather
            lv_obj_t *obj = lv_imagebutton_create(parent_obj);
            objects.img_weather = obj;
            lv_obj_set_pos(obj, 115, 22);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, 70);
            lv_imagebutton_set_src(obj, LV_IMAGEBUTTON_STATE_RELEASED, NULL, &img_icon_weather, NULL);
            lv_imagebutton_set_src(obj, LV_IMAGEBUTTON_STATE_PRESSED, NULL, &img_icon_weather, NULL);
        }
        {
            // label_weather
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.label_weather = obj;
            lv_obj_set_pos(obj, 136, 85);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &ui_font_siyuanheiti_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "天气");
        }
        {
            // img_clock
            lv_obj_t *obj = lv_imagebutton_create(parent_obj);
            objects.img_clock = obj;
            lv_obj_set_pos(obj, 15, 108);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, 70);
            lv_imagebutton_set_src(obj, LV_IMAGEBUTTON_STATE_RELEASED, NULL, &img_icon_clock, NULL);
            lv_imagebutton_set_src(obj, LV_IMAGEBUTTON_STATE_PRESSED, NULL, &img_icon_clock, NULL);
        }
        {
            // label_clock
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.label_clock = obj;
            lv_obj_set_pos(obj, 36, 168);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &ui_font_siyuanheiti_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "时间");
        }
        {
            // img_smart_home
            lv_obj_t *obj = lv_imagebutton_create(parent_obj);
            objects.img_smart_home = obj;
            lv_obj_set_pos(obj, 115, 108);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, 70);
            lv_imagebutton_set_src(obj, LV_IMAGEBUTTON_STATE_RELEASED, NULL, &img_icon_smart_home_manager, NULL);
            lv_imagebutton_set_src(obj, LV_IMAGEBUTTON_STATE_PRESSED, NULL, &img_icon_smart_home_manager, NULL);
        }
        {
            // label_smart_home
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.label_smart_home = obj;
            lv_obj_set_pos(obj, 136, 168);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &ui_font_siyuanheiti_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "家居");
        }
    }
    
    tick_screen_menu();
}

void tick_screen_menu() {
    void *flowState = getFlowState(0, 1);
    (void)flowState;
    tick_user_widget_state_bar(getFlowState(flowState, 0), 4);
}

void create_screen_settings() {
    void *flowState = getFlowState(0, 2);
    (void)flowState;
    lv_obj_t *obj = lv_obj_create(0);
    objects.settings = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 200, 200);
    lv_obj_add_event_cb(obj, event_handler_cb_settings_settings, LV_EVENT_ALL, flowState);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            objects.obj1 = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 200, 20);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            create_user_widget_state_bar(obj, getFlowState(flowState, 0), 8);
        }
        {
            // label_setings
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.label_setings = obj;
            lv_obj_set_pos(obj, 86, -3);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &ui_font_siyuanheiti_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "设置");
        }
        {
            // wlan_settings
            lv_obj_t *obj = lv_obj_create(parent_obj);
            objects.wlan_settings = obj;
            lv_obj_set_pos(obj, 0, 20);
            lv_obj_set_size(obj, 200, 40);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(obj, lv_color_hex(0xff000000), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_LEFT_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "WLAN");
                }
                {
                    // switch_wlan
                    lv_obj_t *obj = lv_switch_create(parent_obj);
                    objects.switch_wlan = obj;
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, 50, 25);
                    lv_obj_add_event_cb(obj, event_handler_cb_settings_switch_wlan, LV_EVENT_ALL, flowState);
                    lv_obj_add_state(obj, LV_STATE_CHECKED);
                    lv_obj_set_style_align(obj, LV_ALIGN_RIGHT_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0xff000000), LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
    }
    
    tick_screen_settings();
}

void tick_screen_settings() {
    void *flowState = getFlowState(0, 2);
    (void)flowState;
    tick_user_widget_state_bar(getFlowState(flowState, 0), 8);
}

void create_user_widget_state_bar(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex) {
    (void)flowState;
    (void)startWidgetIndex;
    lv_obj_t *obj = parent_obj;
    {
        lv_obj_t *parent_obj = obj;
        {
            // state_time
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 0] = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_align(obj, LV_ALIGN_LEFT_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "00:00");
        }
        {
            // state_wifi
            lv_obj_t *obj = lv_image_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 1] = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_wifi_off);
            lv_obj_set_style_align(obj, LV_ALIGN_RIGHT_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            // power_save
            lv_obj_t *obj = lv_image_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 2] = obj;
            lv_obj_set_pos(obj, 172, 3);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(obj, &img_power_save);
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void tick_user_widget_state_bar(void *flowState, int startWidgetIndex) {
    (void)flowState;
    (void)startWidgetIndex;
}


extern void add_style(lv_obj_t *obj, int32_t styleIndex);
extern void remove_style(lv_obj_t *obj, int32_t styleIndex);

static const char *screen_names[] = { "Main", "menu", "settings" };
static const char *object_names[] = { "main", "menu", "settings", "obj0", "obj0__state_time", "obj0__state_wifi", "obj0__power_save", "obj1", "obj1__state_time", "obj1__state_wifi", "obj1__power_save", "img_settings", "switch_wlan", "current_time", "current_weekday", "current_date", "obj2", "obj3", "state_wifi_main", "power_save", "yiyan", "label_menu", "label_settings", "img_weather", "label_weather", "img_clock", "label_clock", "img_smart_home", "label_smart_home", "label_setings", "wlan_settings" };
static const char *style_names[] = { "epaper_button" };


typedef void (*tick_screen_func_t)();
tick_screen_func_t tick_screen_funcs[] = {
    tick_screen_main,
    tick_screen_menu,
    tick_screen_settings,
};
void tick_screen(int screen_index) {
    tick_screen_funcs[screen_index]();
}
void tick_screen_by_id(enum ScreensEnum screenId) {
    tick_screen_funcs[screenId - 1]();
}

void create_screens() {
    eez_flow_init_styles(add_style, remove_style);
    
    eez_flow_init_screen_names(screen_names, sizeof(screen_names) / sizeof(const char *));
    eez_flow_init_object_names(object_names, sizeof(object_names) / sizeof(const char *));
    eez_flow_init_style_names(style_names, sizeof(style_names) / sizeof(const char *));
    
    lv_disp_t *dispp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), false, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);
    
    create_screen_main();
    create_screen_menu();
    create_screen_settings();
}
