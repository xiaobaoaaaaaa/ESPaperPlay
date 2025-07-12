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

#ifndef LV_ATTRIBUTE_IMG_POWER_SAVE
#define LV_ATTRIBUTE_IMG_POWER_SAVE
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_POWER_SAVE
uint8_t img_power_save_map[] = {

    0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0xbb,

    0x00,0x00,
    0x00,0x00,
    0x08,0x00,
    0x1d,0xc0,
    0x3d,0x60,
    0x7c,0x10,
    0x7e,0x18,
    0x74,0x18,
    0x34,0x10,
    0x20,0x30,
    0x4f,0xc0,
    0x00,0x00,
    0x00,0x00,
    0x00,0x00,

};

const lv_image_dsc_t img_power_save = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.flags = 0,
  .header.w = 14,
  .header.h = 14,
  .header.stride = 2,
  .data_size = sizeof(img_power_save_map),
  .data = img_power_save_map,
};

