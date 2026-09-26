#pragma once
#include <stdint.h>
#define LV_COLOR_FORMAT_RGB565 1
typedef struct {struct {uint32_t cf,w,h,stride;} header;uint32_t data_size;const uint8_t *data;} lv_image_dsc_t;
