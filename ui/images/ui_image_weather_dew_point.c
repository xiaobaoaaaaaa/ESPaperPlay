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

#ifndef LV_ATTRIBUTE_IMG_WEATHER_DEW_POINT
#define LV_ATTRIBUTE_IMG_WEATHER_DEW_POINT
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_WEATHER_DEW_POINT
uint8_t img_weather_dew_point_map[] = {

    0x00,0x00,0x00,0x14,0x00,0x00,0x00,0xd8,

    0x00,0x00,0x00,
    0x00,0x00,0x00,
    0x0f,0xe1,0x80,
    0x18,0xc3,0x80,
    0x08,0x66,0xc0,
    0x18,0xe6,0x60,
    0x08,0x44,0x20,
    0x1a,0xe4,0x60,
    0x0a,0xc7,0xe0,
    0x1a,0x41,0x80,
    0x1a,0x60,0x00,
    0x37,0x60,0x00,
    0x2d,0xa0,0x00,
    0x25,0xa0,0x00,
    0x27,0xa0,0x00,
    0x32,0x60,0x00,
    0x18,0xc0,0x00,
    0x0f,0x80,0x00,
    0x00,0x00,0x00,
    0x00,0x00,0x00,

};

const lv_image_dsc_t img_weather_dew_point = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.flags = 0,
  .header.w = 20,
  .header.h = 20,
  .header.stride = 3,
  .data_size = sizeof(img_weather_dew_point_map),
  .data = img_weather_dew_point_map,
};

