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

#ifndef LV_ATTRIBUTE_IMG_WEATHER_VISIBILITY
#define LV_ATTRIBUTE_IMG_WEATHER_VISIBILITY
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_WEATHER_VISIBILITY
uint8_t img_weather_visibility_map[] = {

    0x00,0x00,0x00,0xd8,0x00,0x00,0x00,0x09,

    0xff,0xff,0xf0,
    0xff,0xff,0xf0,
    0xff,0xff,0xf0,
    0xff,0x9f,0xf0,
    0xf8,0x01,0xf0,
    0xf0,0xf8,0xf0,
    0xe3,0x0c,0x70,
    0xce,0x07,0x30,
    0x9c,0xf3,0x90,
    0x9c,0xf3,0x90,
    0x9c,0xf3,0x90,
    0xce,0x47,0x30,
    0xe7,0x0e,0x70,
    0xf1,0xf8,0xf0,
    0xf8,0x01,0xf0,
    0xff,0x07,0xf0,
    0xff,0xff,0xf0,
    0xff,0xff,0xf0,
    0xff,0xff,0xf0,
    0xff,0xff,0xf0,

};

const lv_image_dsc_t img_weather_visibility = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.flags = 0,
  .header.w = 20,
  .header.h = 20,
  .header.stride = 3,
  .data_size = sizeof(img_weather_visibility_map),
  .data = img_weather_visibility_map,
};

