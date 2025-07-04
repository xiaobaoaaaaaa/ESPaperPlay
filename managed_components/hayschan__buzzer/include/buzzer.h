#ifndef __BUZZER_H
#define __BUZZER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/ledc.h"

#define BUZZER_TIMER_DUTY_RES LEDC_TIMER_13_BIT
#define BUZZER_TIMER_SPEED_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_TIMER_SOURCE LEDC_TIMER_0
#define BUZZER_TIMER_FREQ_DEFAULT_KEY_NUM 1

#define BUZZER_CHANNEL_NUM LEDC_CHANNEL_0
// #define BUZZER_CHANNEL_GPIO_NUM 14
#define BUZZER_CHANNEL_SPEED_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_CHANNEL_HPOINT 0
#define BUZZER_CHANNEL_TIMER_SOURCE LEDC_TIMER_0
#define BUZZER_CHANNEL_DEFAULT_DUTY 0 // (2^13)-1 = 8191 8191 * 50% = 4095

typedef enum
{
    NO_NOTE    = 0,
    NOTE_A0    ,
    NOTE_AS0    ,
    NOTE_B0    ,
    NOTE_C1    ,
    NOTE_CS1    ,
    NOTE_D1    ,
    NOTE_DS1    ,
    NOTE_E1    ,
    NOTE_F1    ,
    NOTE_FS1    ,
    NOTE_G1    ,
    NOTE_GS1    ,
    NOTE_A1    ,
    NOTE_AS1    ,
    NOTE_B1    ,
    NOTE_C2    ,
    NOTE_CS2    ,
    NOTE_D2    ,
    NOTE_DS2    ,
    NOTE_E2    ,
    NOTE_F2    ,
    NOTE_FS2    ,
    NOTE_G2    ,
    NOTE_GS2    ,
    NOTE_A2    ,
    NOTE_AS2    ,
    NOTE_B2    ,
    NOTE_C3    ,
    NOTE_CS3    ,
    NOTE_D3    ,
    NOTE_DS3    ,
    NOTE_E3    ,
    NOTE_F3    ,
    NOTE_FS3    ,
    NOTE_G3    ,
    NOTE_GS3    ,
    NOTE_A3    ,
    NOTE_AS3    ,
    NOTE_B3    ,
    NOTE_C4    ,
    NOTE_CS4    ,
    NOTE_D4    ,
    NOTE_DS4    ,
    NOTE_E4    ,
    NOTE_F4    ,
    NOTE_FS4    ,
    NOTE_G4    ,
    NOTE_GS4    ,
    NOTE_A4    ,
    NOTE_AS4    ,
    NOTE_B4    ,
    NOTE_C5    ,
    NOTE_CS5    ,
    NOTE_D5    ,
    NOTE_DS5    ,
    NOTE_E5    ,
    NOTE_F5    ,
    NOTE_FS5    ,
    NOTE_G5    ,
    NOTE_GS5    ,
    NOTE_A5    ,
    NOTE_AS5    ,
    NOTE_B5    ,
    NOTE_C6    ,
    NOTE_CS6    ,
    NOTE_D6    ,
    NOTE_DS6    ,
    NOTE_E6    ,
    NOTE_F6    ,
    NOTE_FS6    ,
    NOTE_G6    ,
    NOTE_GS6    ,
    NOTE_A6    ,
    NOTE_AS6    ,
    NOTE_B6    ,
    NOTE_C7    ,
    NOTE_CS7    ,
    NOTE_D7    ,
    NOTE_DS7    ,
    NOTE_E7    ,
    NOTE_F7    ,
    NOTE_FS7    ,
    NOTE_G7    ,
    NOTE_GS7    ,
    NOTE_A7    ,
    NOTE_AS7    ,
    NOTE_B7    ,
    NOTE_C8    ,
    NOTE_NULL   , 
} piano_note_t;

#ifdef __cplusplus
extern "C"
{
#endif


/**
 * @brief 蜂鸣器初始化，设置分辨率、默认频率、时钟源及工作模式，通道、IO引脚、通道选用的时钟源、默认占空比
*/
esp_err_t buzzer_init(int buzzerPin);

/**
 * @brief 该函数可以调整蜂鸣器响的音符、响度、占空比时间、次数
 * @param current_note 类型: piano_note_t，有91个音符可选
 * @param current_loudness 可通过调节ledc的占空比来调节响度，可选0~8191
 * @param loud_time 一个周期内蜂鸣器响的时间（单位：秒）
 * @param no_loud_time 一个周期内蜂鸣器不响的时间（单位：秒）
 * @param loud_cycle_time 工作周期
*/
void buzzer(piano_note_t current_note, uint32_t current_loudness, float loud_time, float no_loud_time, uint8_t loud_cycle_time);

#ifdef __cplusplus
}
#endif

#endif // BUZZER_H