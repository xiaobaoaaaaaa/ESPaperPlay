#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "buzzer.h"

void buzzer_task(void *pvParameters);

void app_main(void)
{
    buzzer_init(14);
    xTaskCreate(buzzer_task, "buzzer_task", 2048, NULL, 10, NULL);
}

void buzzer_task(void *pvParameters)
{
    while (1) 
    {
        buzzer(NOTE_G4, 7168, 1, 1, 5);
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}    

