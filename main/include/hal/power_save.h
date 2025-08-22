#ifndef _POWER_SAVE_H
#define _POWER_SAVE_H

extern EventGroupHandle_t pwr_save_event_group;

void reset_inactivity_timer();

void power_save_init(void);

#endif