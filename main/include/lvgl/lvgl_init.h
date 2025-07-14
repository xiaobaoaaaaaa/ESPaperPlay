#ifndef _LVGL_INIT_H
#define _LVGL_INIT_H

#include "lvgl.h"

extern lv_display_t *disp;
extern EventGroupHandle_t lvgl_flush_event_group;

void lvgl_init_epaper_display(void);

#endif