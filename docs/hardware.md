# Hardware Overview · 硬件概览

[← README](../README.md)

## English

This repository contains the ESPaperPlay firmware. Hardware information here is
limited to the target platform and the interfaces needed to build and adapt the
software. Schematics, PCB layout, BOM and manufacturing records are maintained
separately in the EasyEDA Pro **ESPaperPlay** project.

| Module | Target hardware / interface |
| --- | --- |
| MCU | ESP32-S3-WROOM-1-N16R8, 16 MB Flash, 8 MB PSRAM |
| Display | GDEY075T7-T01, 7.5-inch 800×480 e-paper, UC8179, SPI |
| Touch | GT911, I²C with interrupt and reset |
| Storage | microSD, SDMMC; bus width and pins are configurable |
| Audio | ICS-43434 microphone and MAX98357A amplifier, I²S |
| Power | USB-C / single-cell Li-ion; firmware-controlled peripheral and audio supplies |
| Controls | BOOT, volume up/down and hardware reset |

GPIO assignments, bus settings and enable polarities are defined in
[`espaperplay_config.h`](../components/board/include/espaperplay_config.h).
Check these settings against the actual board when porting the firmware;
development boards may use different wiring from the custom PCB.
See [Power & Reliability](power.md) for software power management and
[`sdkconfig.defaults`](../sdkconfig.defaults) for ESP-IDF defaults.

## 简体中文

本仓库维护 ESPaperPlay 固件，仅保留目标硬件和软件适配所需的基本说明。
原理图、PCB、BOM 与制造记录在嘉立创 EDA Pro **ESPaperPlay** 工程中单独维护。

| 模块 | 目标硬件 / 接口 |
| --- | --- |
| 主控 | ESP32-S3-WROOM-1-N16R8，16 MB Flash、8 MB PSRAM |
| 显示 | GDEY075T7-T01，7.5 英寸 800×480 电子纸，UC8179，SPI |
| 触摸 | GT911，I²C、中断和复位 |
| 存储 | microSD，SDMMC；总线宽度和引脚可配置 |
| 音频 | ICS-43434 麦克风、MAX98357A 功放，I²S |
| 电源 | USB-C / 单节锂电池；固件控制外设和音频供电 |
| 按键 | BOOT、音量加减及硬件复位 |

GPIO 分配、总线参数和使能电平统一见
[`espaperplay_config.h`](../components/board/include/espaperplay_config.h)。
适配时应对照实际板卡核对配置，开发板接线可能与自研 PCB 不同。
软件电源管理见[电源与可靠性](power.md)，ESP-IDF 默认配置见
[`sdkconfig.defaults`](../sdkconfig.defaults)。
