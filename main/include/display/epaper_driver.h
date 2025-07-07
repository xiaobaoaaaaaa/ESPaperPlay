#ifndef _EPAPER_DRIVER_H
#define _EPAPER_DRIVER_H

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ssd1681.h"
#include "esp_lcd_panel_ops.h"

extern esp_lcd_panel_handle_t panel_handle;

void epaper_init(void);

#endif