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

#ifndef LV_ATTRIBUTE_IMG_WIFI_OFF
#define LV_ATTRIBUTE_IMG_WIFI_OFF
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_WIFI_OFF
uint8_t img_wifi_off_map[] = {

    0x00,0x00,0x00,0xbf,0x00,0x00,0x00,0x0e,

    0xff,0xfc,
    0x9f,0xfc,
    0xc8,0x1c,
    0x87,0xc4,
    0xa3,0xf0,
    0x99,0xe4,
    0xcc,0xcc,
    0xe6,0x5c,
    0xf6,0x3c,
    0xf3,0x1c,
    0xf8,0x4c,
    0xfc,0xe4,
    0xff,0xe4,
    0xff,0xfc,

};

const lv_image_dsc_t img_wifi_off = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.flags = 0,
  .header.w = 14,
  .header.h = 14,
  .header.stride = 2,
  .data_size = sizeof(img_wifi_off_map),
  .data = img_wifi_off_map,
};

