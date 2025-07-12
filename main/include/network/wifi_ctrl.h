#ifndef _WIFI_CTRL_H
#define _WIFI_CTRL_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

extern EventGroupHandle_t s_wifi_event_group;
extern bool wifi_manually_stopped;

bool wifi_init(void);

#endif