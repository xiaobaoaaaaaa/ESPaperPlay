#include "buzzer.h"
#include "esp_pm.h"

// 设置固定频率的函数
void set_fixed_frequency() {
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,  // 设置最大频率为240MHz
        .min_freq_mhz = 240,  // 设置最小频率为240MHz
        .light_sleep_enable = false
    };
    esp_pm_configure(&pm_config);
}

// 恢复动态频率的函数
void restore_dynamic_frequency() {
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,  // 设置最大频率为240MHz
        .min_freq_mhz = 80,   // 设置最小频率为80MHz，允许动态调整
        .light_sleep_enable = false
    };
    esp_pm_configure(&pm_config);
}

const uint16_t notes[]  ={
    0,31,33,35,37,39,41,44,46,49,52,55,58,62,65,69,73,78,82,87,93,98,104,110,117,123,131,139,147,156,165,175,185,196,208,220,
    233,247,262,277,294,311,330,349,370,392,415,440,466,494,523,554,587,622,659,698,740,784,831,880,932,988,1047,1109,1175,1245,1319,1397,1480,1568,1661,
    1760,1865,1976,2093,2217,2349,2489,2637,2794,2960,3136,3322,3520,3729,3951,4186,4435,4699,4978};

ledc_timer_config_t ledc_timer;
ledc_channel_config_t ledc_channel;

esp_err_t buzzer_init(int buzzerPin)
{
    ledc_timer.duty_resolution = BUZZER_TIMER_DUTY_RES;    // resolution of PWM duty
    ledc_timer.freq_hz = 1000;  // frequency of PWM signal, max:5000, when resolution is 13 bits.
    ledc_timer.speed_mode = BUZZER_TIMER_SPEED_MODE;            //  timermode
    ledc_timer.timer_num = BUZZER_TIMER_SOURCE;            // timer index

    // Set configuration of timer0 for high speed channels
    esp_err_t ledc_timer_ret = ledc_timer_config(&ledc_timer);

    ledc_channel.channel    = BUZZER_CHANNEL_NUM;
    ledc_channel.duty       = BUZZER_CHANNEL_DEFAULT_DUTY;
    ledc_channel.gpio_num   = buzzerPin;
    ledc_channel.speed_mode = BUZZER_CHANNEL_SPEED_MODE;
    ledc_channel.hpoint     = BUZZER_CHANNEL_HPOINT;
    ledc_channel.timer_sel  = BUZZER_CHANNEL_TIMER_SOURCE;

    esp_err_t ledc_channel_ret =ledc_channel_config(&ledc_channel);
    if(ledc_channel_ret!=ESP_OK||ledc_timer_ret!=ESP_OK)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void buzzer(piano_note_t current_note, uint32_t current_loudness, float loud_time, float no_loud_time, uint8_t loud_cycle_time)
{
    set_fixed_frequency();
    uint8_t current_loud_cycle_time = 0;
    ledc_set_freq(BUZZER_TIMER_SPEED_MODE, BUZZER_TIMER_SOURCE, notes[current_note]);
    for(current_loud_cycle_time = 0; current_loud_cycle_time < loud_cycle_time; current_loud_cycle_time++)
    {
        ledc_set_duty(BUZZER_TIMER_SPEED_MODE, BUZZER_CHANNEL_NUM, current_loudness);
        ledc_update_duty(BUZZER_TIMER_SPEED_MODE, BUZZER_CHANNEL_NUM);
        vTaskDelay((1000 * loud_time) / portTICK_PERIOD_MS);

        ledc_set_duty(BUZZER_TIMER_SPEED_MODE, BUZZER_CHANNEL_NUM, 0);
        ledc_update_duty(BUZZER_TIMER_SPEED_MODE, BUZZER_CHANNEL_NUM);
        vTaskDelay((1000 * no_loud_time) / portTICK_PERIOD_MS);
    }
    restore_dynamic_frequency();
}
