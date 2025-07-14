#ifndef _LVGL_INIT_H
#define _LVGL_INIT_H

#include "lvgl.h"

extern lv_display_t *disp;
extern SemaphoreHandle_t lvgl_flush_sem;

void lvgl_init_epaper_display(void);

#endif