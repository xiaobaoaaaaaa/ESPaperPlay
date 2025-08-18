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

#ifndef LV_ATTRIBUTE_IMG_WIFI_CONNECTED_LOW
#define LV_ATTRIBUTE_IMG_WIFI_CONNECTED_LOW
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_WIFI_CONNECTED_LOW
uint8_t img_wifi_connected_low_map[] = {

    0x00,0x00,0x00,0x0d,0x00,0x00,0x00,0xd8,

    0x00,0x00,
    0x00,0x00,
    0x1f,0xe0,
    0x70,0x30,
    0x40,0x08,
    0x60,0x18,
    0x30,0x30,
    0x18,0x20,
    0x18,0x40,
    0x0c,0xc0,
    0x07,0x80,
    0x03,0x00,
    0x00,0x00,
    0x00,0x00,

};

const lv_image_dsc_t img_wifi_connected_low = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.flags = 0,
  .header.w = 14,
  .header.h = 14,
  .header.stride = 2,
  .data_size = sizeof(img_wifi_connected_low_map),
  .data = img_wifi_connected_low_map,
};

