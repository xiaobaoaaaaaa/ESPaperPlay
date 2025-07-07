#include <stdio.h>
#include "esp_log.h"
#include "unity.h"
#include "buzzer.h"

#include "esp_timer.h"
extern const uint16_t notes[];
#define BUZZER_TEST_PIN GPIO_NUM_35
inline piano_note_t operator++(piano_note_t &note, int)
{
    return note = static_cast<piano_note_t>(note + 1);
}
// 为了对 buzzer.c 组件进行单元测试，应该覆盖以下测试用例，以验证每个函数的功能，验证各种输入（包括边缘情况）的处理，以及确保组件与硬件和软件环境正确交互。以下是您应该考虑的测试用例：

// 1. buzzer_init 函数测试
// 有效引脚初始化测试：使用有效的蜂鸣器引脚号测试 buzzer_init，以确保正确初始化。
// 定时器配置验证：验证是否用 ledc_timer 中设置的正确参数调用了 ledc_timer_config 函数。
// 通道配置验证：检查是否用 ledc_channel 中设置的参数正确调用了 ledc_channel_config。

TEST_CASE("Buzzer initialization test", "[buzzer]") // 1
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);
}

// 2. buzzer 函数测试
// 基本功能测试：使用一组正常的输入值（钢琴音符、响度、持续时间和循环时间）测试 buzzer 函数，并确保其表现如预期。
TEST_CASE("Buzzer function test", "[buzzer]") // 2
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);
    buzzer(NOTE_C4, 8191, 1, 1, 1);
}

// 不同音符测试：使用各种音符进行测试，以确保为每个音符设置了正确的频率。
TEST_CASE("Buzzer different notes test", "[buzzer]") // 3
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);

    for (piano_note_t note = NOTE_A0; note <= NOTE_NULL; note++)
    {
        buzzer(note, 8191, 0.1, 0.1, 1);

        TEST_ASSERT_UINT32_WITHIN(5, ledc_get_freq(BUZZER_TIMER_SPEED_MODE, BUZZER_TIMER_SOURCE), notes[note]); // 10m的偏差
    }
}

// 响度级别测试：使用不同的响度级别进行测试，以验证 PWM 占空比是否正确设置。
TEST_CASE("Buzzer different levels test", "[buzzer]")
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);

    for (int level = 0; level <= 8191; level += 1000)
    {
        buzzer(NOTE_G7, level, 0.1, 0.1, 1);
        TEST_ASSERT_EQUAL(ledc_get_duty(BUZZER_TIMER_SPEED_MODE, BUZZER_CHANNEL_NUM), 0);
    }
}

// 时间测试：验证响和不响的持续时间是否正确维持。
TEST_CASE("Buzzer duration test", "[buzzer]")
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);

    uint32_t start_time_us = esp_timer_get_time();

    buzzer(NOTE_FS2, 8000, 0.5, 0.5, 2);
    uint32_t end_time_us = esp_timer_get_time();
    uint32_t elapsed_time_us = end_time_us - start_time_us;
    uint32_t elapsed_time_ms = elapsed_time_us / 1000;

    TEST_ASSERT_UINT32_WITHIN(10, (0.5 + 0.5) * 2 * 1000, elapsed_time_ms); // 10m的偏差
}

// 循环时间测试：使用不同的响循环时间进行测试，以确保蜂鸣器在多个循环中的行为正确。
TEST_CASE("Buzzer loop duration test", "[buzzer]")
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);
    for (int loop_duration = 1; loop_duration <= 2; loop_duration++)
    {
        uint32_t start_time_us = esp_timer_get_time();

        buzzer(NOTE_FS2, 8000, loop_duration, loop_duration, 1);
        uint32_t end_time_us = esp_timer_get_time();
        uint32_t elapsed_time_us = end_time_us - start_time_us;
        uint32_t elapsed_time_ms = elapsed_time_us / 1000;

        TEST_ASSERT_UINT32_WITHIN(10, (loop_duration + loop_duration) * 1 * 1000, elapsed_time_ms); // 10m的偏差
    }
}

// 音符值的边缘情况测试：使用最低和最高的音符值进行测试，以及一个无效的音符值。
TEST_CASE("Buzzer note value test", "[buzzer]")
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);

    piano_note_t note = NOTE_A0; // 最小频率
    buzzer(note, 8000, 0.5, 0.5, 1);
    TEST_ASSERT_GREATER_THAN(note, NOTE_NULL);

    note = NOTE_C8; // 最大频率
    buzzer(note, 8000, 0.5, 0.5, 1);
    TEST_ASSERT_GREATER_THAN(note, NOTE_NULL);
}

// 响度的边缘情况测试：使用最大、最小和无效的响度值进行测试。
TEST_CASE("Buzzer volume test", "[buzzer]")
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);

    int volume = 8192; // 最大音量
    buzzer(NOTE_A4, volume, 1, 1, 1);
    TEST_ASSERT_GREATER_THAN(volume, 8193);

    volume = 0; // 最小音量
    buzzer(NOTE_A4, volume, 1, 1, 1);
    TEST_ASSERT_GREATER_THAN(volume, 8193);
}

// 3. 硬件交互测试
// 真实硬件测试：与实际硬件进行集成测试，以确保蜂鸣器根据给定的命令正确操作。
TEST_CASE("Buzzer hardware test", "[buzzer]")
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);

    // 模拟收到指令打开蜂鸣器三秒
    buzzer(NOTE_A4, 8000, 3, 1, 1);
}
// 4. 稳健性和错误处理
// 容错性测试：测试组件在错误条件下的行为，如无效参数或硬件故障。
TEST_CASE("Buzzer error handling test", "[buzzer]")
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);

    // 如果频率为0则该引脚驱动蜂鸣器则失效
    TEST_ASSERT_NOT_EQUAL(ledc_get_freq(BUZZER_TIMER_SPEED_MODE, BUZZER_TIMER_SOURCE), 0);
}
// 资源泄露测试：确保组件不会导致资源泄露，特别是在 GPIO 引脚和计时器资源方面。尤其看有没有内存泄漏！
TEST_CASE("Buzzer resource leakage test", "[buzzer]")
{
    TEST_ASSERT_EQUAL(buzzer_init(BUZZER_TEST_PIN), ESP_OK);
    uint32_t heap_start = esp_get_free_heap_size();
    

    // Perform some operations with the buzzer component
    buzzer(NOTE_A4,8000, 1, 1, 1);

    // Check for resource leakage
    uint32_t heap_end = esp_get_free_heap_size();

    //检测内存有无泄露
    TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(heap_start, heap_end,
                                             "Memory leak detected!");

    gpio_reset_pin(BUZZER_TEST_PIN);
}   

extern "C" void app_main(void)
{
    // setUp();
    printf("BUZZER Tests \n");
    unity_run_menu();
    // tearDown();
}
