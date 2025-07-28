#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_wifi_off;
extern const lv_img_dsc_t img_wifi_connected_low;
extern const lv_img_dsc_t img_wifi_connected_filled;
extern const lv_img_dsc_t img_power_save;
extern const lv_img_dsc_t img_icon_settings;
extern const lv_img_dsc_t img_icon_clock;
extern const lv_img_dsc_t img_icon_smart_home_manager;
extern const lv_img_dsc_t img_icon_weather;
extern const lv_img_dsc_t img_weather_precip;
extern const lv_img_dsc_t img_weather_humidity;
extern const lv_img_dsc_t img_weather_wind;
extern const lv_img_dsc_t img_weather_visibility;
extern const lv_img_dsc_t img_weather_pressure;
extern const lv_img_dsc_t img_weather_dew_point;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[14];


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/