#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "epaper_driver.h"
#include "ssd1681_waveshare_1in54_lut.h"

#include "lv_demos.h"
#include "lvgl_init.h"

#include "config_manager.h"
#include "power_save.h"
#include "touch.h"
#include "ui.h"

#define TAG "lvgl_init"

// LVGL 定时器周期（毫秒）
#define EXAMPLE_LVGL_TICK_PERIOD_MS    50

// 显示屏水平分辨率默认值
#ifndef MY_DISP_HOR_RES
    #define MY_DISP_HOR_RES    200
#endif

// 显示屏垂直分辨率默认值
#ifndef MY_DISP_VER_RES
    #define MY_DISP_VER_RES    200
#endif

// 每个像素的字节数（单色格式）
#define BYTE_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_I1))

// 触摸输入设备指针
static lv_indev_t *indev_touchpad = NULL;
// 显示缓冲区指针
static void *buf1 = NULL, *buf2 = NULL;
// 最大局部刷新次数，超过则触发全屏刷新, 默认为30次
static int max_partial_refresh_count = 30; 
// LVGL 显示对象指针
lv_display_t *disp = NULL;
// 用于阻塞LVGL刷新的事件组
EventGroupHandle_t lvgl_flush_event_group = NULL; 

// 快速刷新计数器，超过 max_partial_refresh_count 则触发全屏刷新
int fast_refresh_count = 0; 

/**
 * @brief 显示刷新回调函数
 * 
 * 此函数由 LVGL 调用，用于将渲染的图像数据刷新到电子墨水屏
 * 
 * @param disp_drv 显示驱动对象
 * @param area 需要刷新的区域
 * @param px_map 像素数据映射
 */
static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t * px_map)
{
    // 计算刷新区域的高度和宽度
    int height = area->y2 - area->y1 + 1;
    int width = area->x2 - area->x1 + 1;

    // 计算像素数据大小（以字节为单位）
    size_t size = height * width / 8;
    // px_map的前8字节是LVGL固定头部，不应被渲染，所以偏移8字节
    uint8_t *buf = px_map + 8; 

    // 创建反转数据缓冲区（屏幕驱动芯片的红色寄存器与黑白寄存器写入相反数据）
    uint8_t inverted[size]; 
    for (unsigned i = 0; i < size; i++) 
    {
        inverted[i] = ~buf[i]; // 对每个字节进行按位取反操作
    }

    ESP_LOGI(TAG, "Flushing area: x1=%d, y1=%d, x2=%d, y2=%d", 
        area->x1, area->y1, area->x2, area->y2);

    // 打开屏幕显示
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // 根据快速刷新计数器决定是否进行局部刷新或全屏刷新
    if (fast_refresh_count < max_partial_refresh_count) 
    {
        // 设置为局部快速刷新模式
        epaper_panel_set_refresh_mode(panel_handle, true);
        fast_refresh_count++;
    } 
    else 
    {
        // 设置为全屏刷新模式
        epaper_panel_set_refresh_mode(panel_handle, false);
        fast_refresh_count = 0; // 重置计数器
    }

    // 分别写入黑白和红色数据到屏幕
    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_BLACK);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, buf);

    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_RED);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, inverted);

    // 刷新屏幕显示
    epaper_panel_refresh_screen(panel_handle);

    // 关闭屏幕显示以节省功耗
    esp_lcd_panel_disp_on_off(panel_handle, false);
}

/**
 * @brief 初始化 LVGL 显示驱动
 * 
 * 创建显示对象并设置缓冲区和刷新回调函数
 */
void lv_port_disp_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL display");

    // 创建显示对象
    disp = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);
    if (disp == NULL) 
    {
        ESP_LOGE(TAG, "Display creation failed");
        return;
    }

    // 设置显示刷新回调函数
    lv_display_set_flush_cb(disp, disp_flush);
    
    // 计算显示缓冲区大小（屏幕大小除以8以保存全部像素，额外申请8字节用于lvgl添加的固定头部）
    size_t buf_size = MY_DISP_HOR_RES * MY_DISP_VER_RES / 8 + 8; 
    // 分配 DMA 内存作为显示缓冲区
    buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);

    if (!buf1 || !buf2) 
    {
        ESP_LOGE(TAG, "Display buffer allocation failed");
        return;
    }

    // 设置显示缓冲区
    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

/**
 * @brief 增加 LVGL 系统时钟滴答
 * 
 * 由定时器回调函数定期调用，更新 LVGL 内部时钟
 * 
 * @param arg 定时器参数（未使用）
 */
static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

/**
 * @brief LVGL 定时器任务
 * 
 * 处理 LVGL 定时器事件并更新 UI
 * 
 * @param param 任务参数（未使用）
 */
void lvgl_timer_task(void *param)
{
    while (1) {
        // 等待 LVGL 刷新事件
        EventBits_t bits = xEventGroupWaitBits(lvgl_flush_event_group, BIT0, 
                         pdFALSE, pdFALSE, pdMS_TO_TICKS(500));

        if (bits & BIT0) 
        {
            // 处理 LVGL 定时器事件
            lv_timer_handler();
            // 更新 UI
            ui_tick();
        }

        // 延迟一个周期
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_LVGL_TICK_PERIOD_MS));
    }
}

// 触摸数据结构体
typedef struct {
    int32_t x;           // X 坐标
    int32_t y;           // Y 坐标
    bool is_pressed;     // 是否按下
    SemaphoreHandle_t mutex; // 互斥锁，用于保护数据访问
} touch_data_t;

// 全局触摸数据对象
static touch_data_t g_touch_data = {
    .x = 0,
    .y = 0,
    .is_pressed = false,
    .mutex = NULL
};

/**
 * @brief 触摸屏读取任务
 * 
 * 异步读取触摸屏数据并更新全局触摸数据
 * 
 * @param param 任务参数（未使用）
 */
static void touch_read_task(void *param)
{
    while (1) {
        // 读取触摸屏数据
        ctp_tp_t ctp;
        i2c_ctp_FTxxxx_read_all(I2C_NUM_0, &ctp);

        // 获取触摸数据互斥锁
        xSemaphoreTake(g_touch_data.mutex, portMAX_DELAY);
        
        // 如果检测到触摸点
        if (ctp.tp_num > 0) {
            // 转换触摸屏 X 轴坐标
            int32_t x = ((int16_t)(ctp.tp[0].x - 160) - 319) * -1; 
            int32_t y = ctp.tp[0].y; // Y 轴坐标
            
            // 检查坐标是否在屏幕范围内
            if (x < 200 && y < 200) {
                g_touch_data.x = x;
                g_touch_data.y = y;
                g_touch_data.is_pressed = true;
                reset_inactivity_timer(); // 检测到触摸后重置睡眠计数器
            } else {
                g_touch_data.is_pressed = false;
            }
        } else {
            g_touch_data.is_pressed = false;
        }
        
        // 释放触摸数据互斥锁
        xSemaphoreGive(g_touch_data.mutex);
        // 延迟 20ms
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/**
 * @brief 触摸板读取回调函数
 * 
 * 由 LVGL 调用，用于读取当前触摸状态
 * 
 * @param indev_drv 输入设备驱动对象
 * @param data 输入设备数据结构体
 */
static void touchpad_read(lv_indev_t * indev_drv, lv_indev_data_t * data)
{
    // 获取触摸数据互斥锁
    xSemaphoreTake(g_touch_data.mutex, portMAX_DELAY);
    
    // 设置触摸状态（按下或释放）
    data->state = g_touch_data.is_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    // 设置触摸点坐标
    data->point.x = g_touch_data.x;
    data->point.y = g_touch_data.y;
    
    // 释放触摸数据互斥锁
    xSemaphoreGive(g_touch_data.mutex);
}

/**
 * @brief 初始化 LVGL 输入设备（触摸屏）
 * 
 * 创建触摸输入设备并设置读取回调函数
 */
void lv_port_indev_init(void)
{
    // 创建触摸数据互斥锁
    g_touch_data.mutex = xSemaphoreCreateMutex();
    if (g_touch_data.mutex == NULL) 
    {
        ESP_LOGE(TAG, "Touch mutex creation failed");
        return;
    }

    // 创建触摸读取任务，绑定到核心0
    xTaskCreatePinnedToCore(touch_read_task, "touch_task", 2048, NULL, 5, NULL, 0);

    // 创建输入设备对象
    indev_touchpad = lv_indev_create();
    // 设置输入设备类型为指针设备
    lv_indev_set_type(indev_touchpad, LV_INDEV_TYPE_POINTER);
    // 设置输入设备读取回调函数
    lv_indev_set_read_cb(indev_touchpad, touchpad_read);
}

/**
 * @brief 初始化电子墨水屏显示
 * 
 * 初始化电子墨水屏驱动、LVGL 显示和输入设备
 */
void lvgl_init_epaper_display(void)
{
    // 创建 LVGL 刷新事件组并设置初始位
    lvgl_flush_event_group = xEventGroupCreate();
    xEventGroupSetBits(lvgl_flush_event_group, BIT0);

    // 获取系统配置参数
    const system_config_t *config = config_get();
    if (config != NULL) 
    {
        max_partial_refresh_count = config->max_partial_refresh_count;
    } 
    else 
    {
        ESP_LOGW(TAG, "Failed to get system configuration, using default values");
    }

    // 初始化电子墨水屏硬件
    epaper_init();
    // 初始化 LVGL 库
    lv_init();
    // 初始化显示驱动
    lv_port_disp_init();
    // 初始化输入设备
    lv_port_indev_init();

    ESP_LOGI(TAG, "Setting up LVGL tick timer");
    // 配置 LVGL 系统时钟定时器
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick, // 定时器回调函数
        .name = "lvgl_tick"              // 定时器名称
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    // 初始化 UI
    ui_init();
    // 创建 LVGL 定时器任务
    xTaskCreate(lvgl_timer_task, "lvgl_task", 8192, NULL, 10, NULL);
}