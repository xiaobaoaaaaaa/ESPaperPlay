#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
#include "lvgl.h"
#elif defined(LV_BUILD_TEST)
#include "../lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif


#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_WIFI_CONNECTED_FILLED
#define LV_ATTRIBUTE_IMG_WIFI_CONNECTED_FILLED
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_WIFI_CONNECTED_FILLED
uint8_t img_wifi_connected_filled_map[] = {

    0x00,0x00,0x00,0x0a,0x00,0x00,0x00,0xf7,

    0x00,0x00,
    0x00,0x00,
    0x0f,0xe0,
    0x3f,0xf8,
    0x7f,0xfc,
    0x3f,0xfc,
    0x3f,0xf8,
    0x0f,0xf0,
    0x0f,0xe0,
    0x07,0xc0,
    0x03,0x80,
    0x01,0x00,
    0x00,0x00,
    0x00,0x00,

};

const lv_image_dsc_t img_wifi_connected_filled = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.flags = 0,
  .header.w = 15,
  .header.h = 14,
  .header.stride = 2,
  .data_size = sizeof(img_wifi_connected_filled_map),
  .data = img_wifi_connected_filled_map,
};

