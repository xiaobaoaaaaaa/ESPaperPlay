/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "espaperplay_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_epd.h
 * @brief 电子纸显示屏（EPD）抽象层（GDEY075T7-T01 / UC8179）。
 *
 * 本组件实现 GDEY075T7-T01（7.5"，800x480，UC8179 控制器，SPI 接口）驱动：
 * SPI 设备注册、硬件复位、面板初始化（PSR / PON / CDI 等）、全屏与局部
 * （Partial Window）刷新、深度睡眠与电源轨关断。
 *
 * 实现依据：
 *  - 《GDEY075T7-T01 规格书》第 7 章命令表（MinerU 转换版 Markdown 在仓库根目录）；
 *  - 厂商随板参考工程 S-GDEY075T7-FP-GT911Touch20230713（HARDWARE/EPD/
 *    Display_EPD_W21.c，UC8179 时序已上板验证）。
 *
 * 关键行为约定：
 *  - 像素极性：数据位 1 = 白（0xFF），0 = 黑（0x00）——与参考工程 GUI_Paint.h
 *    的 WHITE=0xFF / BLACK=0x00 一致（KW 模式，CDI DDX=01）；
 *  - 差分刷新（N2OCP）：CDI 命令的 N2OCP 位使控制器在每次刷新完成后自动把
 *    新图像平面（DTM2）拷贝到旧图像平面（DTM1），驱动无需软件维护"上一帧"。
 *    每次刷新只写 DTM2，控制器按真实的 {旧,新} 组合选择 LUT 波形——局部刷新
 *    可作用于任意背景（黑底画白、白底画黑均可），且无地址对齐伪影；
 *  - 清屏（image_buf=NULL）：只写 DTM2=0xFF，保留 DTM1——黑像素经历 黑->白
 *    深度擦除波形，白像素保持不动（避免整屏无谓翻转）；
 *  - 4 灰阶（读图用）：UC8179 出厂四灰阶波形（强制温度 0x5F + PWR/BTST 配置，
 *    同 idfxx 驱动在 GDEY075T7 上的取值），两张图像平面各存 1 bit——刷新时
 *    DTM1/DTM2 分别写入灰阶值 bit0/bit1 的取反（控制器 RAM (1,1)=白）。
 *    灰阶只支持全屏刷新（灰阶波形无法局部驱动），刷新更慢、残影更多，
 *    建议仅用于图片类内容；
 *  - 快刷（API 兼容保留）：与全屏模式相同的 OTP 快刷波形。实测 PLL 帧率
 *    对本面板无效（规格书 FRS 表与 uc8151 同族编码的全部候选值耗时相同，
 *    波形按时间而非帧数计时），快刷与全屏耗时相同；后续提速方向是注册表
 *    LUT 或局部波形整屏刷新，另行验证；
 *  - 从灰阶切回黑白/快刷时，旧平面（DTM1）会被写成"新帧的反相"：每个像素
 *    的 {旧,新} 必为相反值，白像素强制 K2W、黑像素强制 W2K——面板上残留的
 *    中间灰被彻底清除（若仅置"旧=全白"，白像素走 LUTWW 不驱动，灰点会残留）。
 *    该转换随黑白刷新自动完成，上层无需感知；局部窗口同样处理（窗口内反相）。
 *  - 局部刷新：CDI(0xA9,0x07) -> PTIN(0x91) -> PTL(0x90, 窗口, 终点含端点)
 *    -> DTM2(0x13) -> DRF(0x12) -> 等 BUSY -> PTOUT(0x92)（DRF 后延时 10ms
 *    再轮询，防止 BUSY 未拉低导致提前返回；PTOUT 在 DRF 后为官方 demo 顺序，
 *    实测比 DRF 前退出快 0~13%，小窗口收益最大）。x 与 width 必须为 8 的倍数
 *    （1bpp 按字节寻址）；
 *  - 初始化时对两个图像平面做全白基线（init 内 PON 后写入），保证首次差分
 *    刷新有确定起点（假设上电时面板为白，同 idfxx 驱动在 GDEY075T7 上的
 *    做法）；每次刷新前重新初始化控制器（硬件复位 + PON），图像 RAM 在
 *    复位/深度睡眠后保留，因此「睡眠唤醒后无需额外调用 init」；
 *  - 刷新完成后面板保持上电；驱动提供空闲自动睡眠保底（最后一次刷新后
 *    超过 ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS 无新刷新即自动深度睡眠，
 *    置 0 关闭）。上层仍可在合适的时机显式调用 espaperplay_epd_sleep()
 *    （如进入设备级睡眠时，由 power 服务统一编排）。
 *
 * 硬件引脚与 SPI 参数集中在 espaperplay_config.h。EPD 与 SD 卡共用 SPI2
 * 主机，主机总线由 board 组件初始化；本组件仅注册自己的 SPI 设备，并自行
 * 管理 DC / RST / BUSY / PWR 引脚（与 touch 组件的做法一致）。
 */

/**
 * @brief EPD 刷新模式。
 */
typedef enum {
    ESPAPERPLAY_EPD_MODE_FULL = 0, /*!< 全屏刷新（1bpp，OTP 波形，较慢，对比度更高） */
    ESPAPERPLAY_EPD_MODE_FULL_FORCE, /*!< 强制全像素翻转全刷（DTM1=~新帧，全像素深波形，
                                      清残影/鬼影；画面会闪黑一下，周期性使用） */
    ESPAPERPLAY_EPD_MODE_PARTIAL,  /*!< 局部 / 快速刷新（1bpp，屏幕不闪烁，区域 8 像素对齐） */
    ESPAPERPLAY_EPD_MODE_GRAY4,    /*!< 4 灰阶全屏刷新（2bpp，仅全屏，不支持局部） */
    ESPAPERPLAY_EPD_MODE_FAST,     /*!< 快刷（1bpp 全屏，注册表 LUT 短波形，刷新最快、残影略多） */
    ESPAPERPLAY_EPD_MODE_MAX,
} espaperplay_epd_mode_t;

/**
 * @brief 空闲自动睡眠超时（毫秒）——初始默认值。
 *
 * 最后一次刷新后若在此时长内没有新的刷新调用，驱动自动让面板进入深度
 * 睡眠（保底机制：防止上层忘记睡眠导致面板长期带电；参考工程要求刷新
 * 后必须睡眠）。刷新会自动唤醒（重新初始化），因此本保底不影响正确性，
 * 仅在下一次刷新时增加约 300ms 的唤醒初始化开销。置 0 关闭自动睡眠
 * （改由上层显式调用 espaperplay_epd_sleep()）。
 *
 * 运行期可用 espaperplay_epd_set_idle_sleep_timeout_ms() 调整（Web 控制台
 * 可配置并经 NVS 持久化）；本宏仅作为上电初始值。
 */
#ifndef ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS
#define ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS 30000
#endif

/**
 * @brief 设置空闲自动睡眠超时（毫秒，0 = 关闭自动睡眠）。
 *
 * 立即生效：若面板正处于空闲计时（定时器运行中），会用新值重新武装；
 * 置 0 时取消待触发的自动睡眠。持久化由上层（系统配置 / Web）负责，
 * 本函数只改运行期行为。
 *
 * @param timeout_ms 超时毫秒数（0 表示关闭）。
 *
 * @return 成功返回 ESP_OK；未初始化返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_epd_set_idle_sleep_timeout_ms(uint32_t timeout_ms);

/**
 * @brief 获取当前空闲自动睡眠超时（毫秒，0 = 关闭）。
 *
 * @return 当前超时值。
 */
uint32_t espaperplay_epd_get_idle_sleep_timeout_ms(void);

/**
 * @brief 启动自检任务（驱动验收用，默认关闭）。
 *
 * 使能后，espaperplay_epd_init() 会创建一个后台任务，依次执行：全屏清白、
 * 全屏测试图案（左半屏黑）、局部刷新（两个方向）、4 灰阶色带、局刷大小
 * 对照、空闲自动睡眠验证，最后刷成全白并进入睡眠，同时打印各模式耗时。
 * 驱动验收完成后置 0 关闭（默认）；需要复验时改回 1 重新编译即可。
 */
#ifndef ESPAPERPLAY_EPD_ENABLE_SELFTEST
#define ESPAPERPLAY_EPD_ENABLE_SELFTEST 0
#endif

/**
 * @brief 初始化时是否使能 EPD 电源轨（ESPAPERPLAY_PIN_EPD_PWR 拉高）。
 */
#ifndef ESPAPERPLAY_EPD_ENABLE_POWER_PIN
#define ESPAPERPLAY_EPD_ENABLE_POWER_PIN 1
#endif

/**
 * @brief 等待 BUSY 释放的超时时间（毫秒）。
 *
 * 全屏刷新典型耗时约 2~3s（规格书未给出精确值），此处取 10s 作为安全上限；
 * 超时返回 ESP_ERR_TIMEOUT。
 */
#ifndef ESPAPERPLAY_EPD_BUSY_TIMEOUT_MS
#define ESPAPERPLAY_EPD_BUSY_TIMEOUT_MS 10000
#endif

/**
 * @brief 初始化电子纸显示屏。
 *
 * 完成电源轨上电、DC/RST/BUSY GPIO 配置、SPI 设备注册与硬件复位，随后使
 * 面板进入深度睡眠（低功耗待机）。第一次调用 espaperplay_epd_refresh()
 * 时会自动完成控制器初始化（PSR/PON），无需额外步骤。
 *
 * @note 本函数可安全重复调用；esp_epd_power_off() 之后再次调用可重新上电。
 *
 * @return 成功返回 ESP_OK；SPI 设备注册或硬件初始化失败返回相应错误码。
 */
esp_err_t espaperplay_epd_init(void);

/**
 * @brief 刷新电子纸显示屏。
 *
 * @param image_buf 图像缓冲指针（左上角为原点）。
 *                   全屏/局部/快刷模式：1 bpp，数据位 1=白 / 0=黑；
 *                   全屏整帧 48000 字节，局部窗口 width*height/8 字节；
 *                   灰阶模式：2 bpp，每像素 2bit（MSB 在前），
 *                   0=白 / 1=浅灰 / 2=深灰 / 3=黑，整帧 96000 字节，
 *                   忽略 x/y/width/height；
 *                   传 NULL 表示执行"清屏（全白）"刷新（各模式通用）。
 * @param x          区域左上角 X 坐标（仅局部模式使用，须为 8 的倍数）。
 * @param y          区域左上角 Y 坐标（仅局部模式使用）。
 * @param width      区域宽度（像素；仅局部模式使用，须为 8 的倍数）。
 * @param height     区域高度（像素；仅局部模式使用）。
 * @param mode       刷新模式：全屏 / 局部 / 4 灰阶 / 快刷（后两者仅全屏）。
 *
 * @return 成功返回 ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；BUSY 超时返回
 *         ESP_ERR_TIMEOUT；其余底层错误返回相应错误码。
 */
esp_err_t espaperplay_epd_refresh(const void *image_buf, uint16_t x, uint16_t y, uint16_t width,
                                  uint16_t height, espaperplay_epd_mode_t mode);

/**
 * @brief 使电子纸显示屏进入深度睡眠。
 *
 * 依次执行：CDI 睡眠设置 -> POF（关断内部电源，等待 BUSY）-> DSLP(0xA5)。
 * 面板保留当前图像；再次刷新时驱动会自动硬件复位唤醒并重新初始化。
 *
 * @return 成功返回 ESP_OK；BUSY 超时返回 ESP_ERR_TIMEOUT；未初始化返回
 *         ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_epd_sleep(void);

/**
 * @brief 完全关闭电子纸显示屏的电源轨。
 *
 * 若面板尚处于上电状态，先执行深度睡眠，再拉低 ESPAPERPLAY_PIN_EPD_PWR。
 * 再次使用前需调用 espaperplay_epd_init() 重新上电。
 *
 * @return 成功返回 ESP_OK，否则返回相应错误码。
 */
esp_err_t espaperplay_epd_power_off(void);

#ifdef __cplusplus
}
#endif
