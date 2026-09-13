# Hardware Design · 硬件设计

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

> This page documents the hardware **as designed and verified** — schematic
> v2.0 of the five-page EasyEDA Pro project (POWER / MCU / EPD / TOUCH / SD).
> It records the power architecture, the parts on each page, and the MCU pin
> allocation the firmware depends on. The EasyEDA project (*ESPaperPlay*)
> remains the single source of truth for the schematic, PCB and BOM; this page
> is a derived overview kept in sync with it.
>
> 本文记录**已设计并验证完成的硬件**——嘉立创 EDA Pro 五页工程（POWER / MCU /
> EPD / TOUCH / SD）的 v2.0 原理图：电源架构、各页器件构成、以及固件依赖的
> 主控引脚分配。原理图 / PCB / BOM 的编辑以嘉立创 EDA 工程（*ESPaperPlay*）
> 为唯一事实源，本页是其保持同步的概览。

---

<a id="en"></a>
## English

### System Overview

A custom carrier board for the ESPaperPlay firmware:

- **SoC** — ESP32-S3-WROOM-1-N16R8 (16 MB flash + 8 MB octal PSRAM, LCSC
  C2913202).
- **Display** — 7.5″ 800×480 e-paper **GDEY075T7-T01** (UC8179), 24-pin FPC,
  with **GT911** capacitive touch on its separate 6-pin FPC.
- **Storage** — push-push **microSD** slot, 4-bit SDMMC.
- **Power** — USB-C 5 V input, single-cell Li-ion on a PH2.0 header; an
  always-on 3.3 V main rail plus a firmware-switched 3.3 V peripheral rail.

The core idea of the v2.0 board: **the MCU is always powered**. "Off" means
deep sleep with the peripheral rail cut — so flashing, resets and crashes
never lose power, and the shutdown path is purely software.

### Power Architecture

```
USB-C 5 V (VBUS) ─┬─ TP4056 ────────────────► VBAT (Li-ion, PH2.0)
                  ├─ SS34 ────────► VRAW ◄──── AO3401A (Q3) ◄── VBAT
                  │                    │        (battery load switch)
                  │                    ▼
                  │            TPS62840 buck ──► 3V3  (main rail, always on)
                  │            (L2 2.2 µH)
                  └─ LED charge indicator
3V3 ── AO3401A (Q2, gate ← POWER_EN) ──► 3V3_PER  (EPD / touch / SD)
```

- **Input & protection** — Type-C 16-pin receptacle (C2765186) with 5.1 kΩ
  CC pull-downs (R11/R12) per UFP requirements; VBUS ESD PESD5V0S1UB (D4);
  USB data-line ESD USBLC6-2SC6 (D5) in straight-through flow-through wiring.
- **Charging** — TP4056 (C725790), PROG = 1.2 kΩ (≈1 A), CE pulled to VBUS
  via 10 kΩ (always enabled when USB is present). CHRG#/STDBY# are
  open-drain with 100 kΩ pull-ups to 3V3 and are read by the MCU; the charge
  LED hangs off VBUS, so charging remains visible while the system is "off".
  Battery connects through a PH2.0-2P header (H1, C3029440).
- **Load sharing** — while VBUS is present, Q3's gate is lifted toward VBUS
  (R18/R19), turning the battery path off; VRAW is fed from VBUS through
  SS34 (D6). When USB is unplugged Q3 conducts seamlessly — the system never
  resets.
- **Main rail** — TPS62840DLCR (C2071859): EN tied to VIN (always on),
  MODE/STOP to GND, VSET = 267 kΩ → 3.3 V, 750 mA. Power inductor
  **PNR4030-2R2-N** (L2, 2.2 µH, Isat 3.8 A, C55381752). Combined output
  capacitance on 3V3 + 3V3_PER sits inside the part's allowed COUT window.
- **Peripheral rail** — Q2 (AO3401A) high-side switch, 3V3 → 3V3_PER. Gate
  network: 4.7 kΩ from **POWER_EN** (GPIO41, low = rail on), 100 kΩ pull-up
  to 3V3 (off at reset — safe default), 1 µF gate capacitor for a ~5 ms
  soft start.
- **Monitoring** — battery divider R23/R24 = 10 M/3.3 M (ratio 0.248, with
  100 nF filter) → **BAT_SENSE** (GPIO1, ADC1_CH0); VBUS divider R25/R26 =
  1 M/1.5 M (ratio 0.6, high = present) → **USB_PRES** (GPIO6); charge state
  → **CHRG_STAT** (GPIO39) / **STANDBY_STAT** (GPIO40).
- **Shutdown floor** — ≈14 µA with the battery in place (MCU deep sleep
  ≈8 µA, charger sleep, divider leakage ≈0.3 µA).

### Soft Power & Buttons

Both buttons are side-push tact switches (TS24CA, C393942):

- **BOOT (SW2)** — GPIO0 with 10 kΩ pull-up. Short press wakes the device
  from deep sleep; a long press (≥3 s, firmware-defined) is the graceful
  "off" command (unmount SD, save state, cut 3V3_PER, deep sleep).
- **RESET (SW1)** — on the EN pin with 10 kΩ pull-up (R27) and 1 µF (C27)
  for a clean ~10 ms power-up ramp; resets the MCU without ever cutting
  power.
- Because the MCU supply is never interrupted, reset and flashing sessions
  never lose the "on" state.

### MCU Page — Pin Allocation

The allocation below is what the firmware depends on
(`components/board/include/espaperplay_config.h` is kept in sync):

| Function | GPIO | Module pin | Net | Remote end |
|---|---|---|---|---|
| BOOT key | IO0 | 27 | BOOT_BTN | SW2 + R28 10 kΩ |
| Battery sense (ADC1_CH0) | IO1 | 39 | BAT_SENSE | R23/R24 divider + C24 |
| Touch reset | IO2 | 38 | RST | FPC2.3 + R5 10 kΩ pull-up |
| Touch interrupt | IO3 | 15 | INT | FPC2.4 |
| Touch I²C data | IO4 | 4 | SDA | FPC2.5 + R3 10 kΩ |
| Touch I²C clock | IO5 | 5 | SCL | FPC2.6 + R4 10 kΩ |
| VBUS presence | IO6 | 6 | USB_PRES | R25/R26 divider |
| EPD busy | IO7 | 7 | BUSY | FPC1.9 |
| EPD reset | IO8 | 12 | RES | FPC1.10 |
| EPD data/command | IO9 | 17 | D/C | FPC1.11 |
| EPD chip select | IO10 | 18 | CS | FPC1.12 |
| EPD SPI MOSI | IO11 | 19 | SDI | FPC1.14 |
| EPD SPI clock | IO12 | 20 | SCLK | FPC1.13 |
| SD clock | IO14 | 22 | SD_CLK | CARD1.5 |
| SD command | IO15 | 8 | SD_CMD | CARD1.3 + R8 10 kΩ |
| SD data 0 | IO16 | 9 | SD_D0 | CARD1.7 + R9 10 kΩ |
| SD data 1 | IO17 | 10 | SD_D1 | CARD1.8 + R10 10 kΩ |
| SD data 2 | IO18 | 11 | SD_D2 | CARD1.1 + R6 10 kΩ |
| USB D− | IO19 | 13 | USB_DM | D5.4 → USB1 |
| USB D+ | IO20 | 14 | USB_DP | D5.6 → USB1 |
| SD data 3 | IO21 | 23 | SD_D3 | CARD1.2 + R7 10 kΩ |
| Charging indicator | IO39 | 32 | CHRG_STAT | U1.7 (TP4056 CHRG#) |
| Charge-done indicator | IO40 | 33 | STANDBY_STAT | U1.6 (TP4056 STDBY#) |
| Peripheral-rail enable | IO41 | 34 | POWER_EN | R22 → Q2 gate |

- **EN** (pin 3) — R27 10 kΩ pull-up + C27 1 µF + SW1 (reset).
- **Deliberately unconnected** — IO35/36/37 (pins 28–30) belong to the
  octal PSRAM inside the module and must stay open; RXD0/TXD0 (36/37 pins)
  are reserved for UART0 debugging; IO13/38/42/45/46/47/48 are spare.
- Module power: GND (1/40/41), 3V3 (2) with C25 10 µF + C26 100 nF
  decoupling.

### EPD Page

24-pin FPC connector FPC1 (HOAUC C53436608) — pinout verified against the
official Good Display GDEY075T7-T01 specification and CAD drawing:

- Logic pins 9–14 map 1:1 to BUSY / RES / D/C / CS / SCLK / SDI.
- **BS1 (pin 8) → GND** selects 4-wire SPI.
- VDDIO/VCI (pins 15/16) come from **3V3_PER**; VDD (18) carries its 1 µF
  to GND; VPP (19) and the temperature-sensor pins TSCL/TSDA (6/7) are open.
- The positive/negative gate and VCOM rails are generated by the discrete
  booster on this page — L1 10 µH (FHD252012S-100MT, C602018) from 3V3_PER
  into Q1 (SI1308EDL, C7603347), a gate-drive switch driven by the panel's
  GDR output (pin 2) with RESE current sense (pin 3, R2 0.47 Ω); MBR0530
  Schottkys (D1–D3) and pump caps produce VGH (21) / VGL (23); VCOM (24),
  VSHR/VSH1/VSL (5/20/22) carry their decoupling caps.

### TOUCH Page

6-pin FPC connector FPC2 (LAILAN C55172961) — pin order matches the official
GDEY075T7-T01 CAD drawing exactly: **GND / VCC / RESET / INT / SDA / SCL**.
VCC hangs on 3V3_PER; I²C pull-ups (R3/R4, 10 kΩ) are on the same rail; RST
has a 10 kΩ pull-up (R5) so the GT911 comes out of reset whenever the MCU
pin is high-Z — this protects the controller's I²C address latch (a known
failure mode handled by the driver's self-healing loop, see
[Power & Reliability](power.md)).

### SD Page

Push-push microSD slot CARD1 (SHOU HAN C393941) in standard pin order,
4-bit SDMMC on GPIO14–18 + 21. VDD is on 3V3_PER with 10 µF + 100 nF; CMD
and DAT0–DAT3 carry 10 kΩ pull-ups to 3V3_PER (CLK intentionally has none);
the card-detect switch is unused.

### Verification Status

- Whole-project strict **DRC: 0 errors**; every net has both ends (no
  dangling nets, no duplicate designators, all 77 components verified in the
  netlist).
- Cross-page nets (EPD/TOUCH/SD signals, USB pair, POWER_EN, dividers,
  charge status) verified closed by netlist extraction.
- FPC1/FPC2 pin maps verified against official Good Display documentation;
  charger/DCDC/load-switch pinouts verified against datasheets.
- USB ESD channel pairing verified against the ST pin-configuration figure
  (D5: I/O1 = pins 1/6, I/O2 = pins 3/4) — D+/D− pass straight through.
- Firmware pin map synced in `espaperplay_config.h`; octal PSRAM enabled
  (`sdkconfig.defaults`) with its reserved pins untouched.

**Before ordering PCBs** — remaining manual check:

1. TS24CA button pin grouping: with a multimeter, unpressed, the signal pin
   must be open against all three grounded pins; pressed, it must conduct
   (the datasheet does not show the internal grouping).

---

<a id="zh"></a>
## 简体中文

### 系统概览

ESPaperPlay 固件的自研载板：

- **主控**——ESP32-S3-WROOM-1-N16R8（16 MB Flash + 8 MB 八线 PSRAM，
  立创 C2913202）。
- **显示**——7.5″ 800×480 电子纸 **GDEY075T7-T01**（UC8179），24P FPC；
  **GT911** 电容触摸经独立的 6P FPC 引出。
- **存储**——自弹式 **microSD** 卡座，4 位 SDMMC。
- **电源**——Type-C 5V 输入，单节锂电池（PH2.0 座）；常开 3.3V 主轨 +
  固件开关的 3.3V 外设轨。

v2.0 板的核心思路：**主控永远带电**。"关机"= 深睡 + 外设轨断电——烧录、
复位、崩溃都不会掉电，关机完全由软件实现。

### 电源架构

```
Type-C 5V (VBUS) ─┬─ TP4056 ────────────────► VBAT（锂电池，PH2.0）
                  ├─ SS34 ────────► VRAW ◄──── AO3401A（Q3）◄── VBAT
                  │                    │        （电池负载开关）
                  │                    ▼
                  │            TPS62840 降压 ──► 3V3（主轨，常开）
                  │            （L2 2.2µH）
                  └─ 充电指示灯
3V3 ── AO3401A（Q2，栅极 ← POWER_EN）──► 3V3_PER（EPD / 触摸 / SD）
```

- **输入与防护**——Type-C 16P 母座（C2765186），CC 双 5.1K 下拉（R11/R12）
  符合受电设备要求；VBUS ESD 选用 PESD5V0S1UB（D4）；USB 数据线 ESD 选用
  USBLC6-2SC6（D5），按 1↔6 / 3↔4 直通方式接线。
- **充电**——TP4056（C725790），PROG = 1.2K（约 1A），CE 经 10K 上拉至
  VBUS（USB 在位即使能）。CHRG#/STDBY# 开漏输出、100K 上拉至 3V3 并接入
  主控回读；充电指示灯挂 VBUS——系统"关机"时充电状态依旧可见。电池经
  PH2.0-2P 卧贴座（H1，C3029440）接入。
- **负载共享**——VBUS 在位时 Q3 栅极被拉向 VBUS（R18/R19），电池通路关断，
  VRAW 由 VBUS 经 SS34（D6）馈入；拔线后 Q3 无缝导通，系统不复位。
- **主轨**——TPS62840DLCR（C2071859）：EN 接 VIN（常开），MODE/STOP 接地，
  VSET = 267K → 3.3V，750mA；功率电感 **PNR4030-2R2-N**（L2，2.2µH、
  Isat 3.8A，C55381752）。3V3 与 3V3_PER 的合计输出电容在器件允许的
  COUT 窗口内。
- **外设轨**——Q2（AO3401A）高侧开关，3V3 → 3V3_PER。栅极网络：4.7K 接
  **POWER_EN**（GPIO41，低电平开轨）、100K 上拉至 3V3（复位默认关，安全）、
  1µF 栅极电容提供约 5ms 软启动。
- **监测**——电池分压 R23/R24 = 10M/3.3M（比例 0.248，100nF 滤波）→
  **BAT_SENSE**（GPIO1，ADC1_CH0）；VBUS 分压 R25/R26 = 1M/1.5M（比例 0.6，
  高 = 在位）→ **USB_PRES**（GPIO6）；充电状态 → **CHRG_STAT**（GPIO39）/
  **STANDBY_STAT**（GPIO40）。
- **关机底电流**——电池在位约 14µA（主控深睡约 8µA + 充电睡眠 + 分压
  漏电约 0.3µA）。

### 软开关与按键

两颗按键均为侧按轻触开关（TS24CA，C393942）：

- **BOOT（SW2）**——GPIO0、10K 上拉。短按从深睡唤醒；长按（≥3s，固件
  定义）执行优雅"关机"（卸载 SD、保存状态、断 3V3_PER、进入深睡）。
- **RESET（SW1）**——接 EN 脚，10K 上拉（R27）+ 1µF（C27）保证上电
  约 10ms 平滑爬升；只复位主控、绝不断电。
- 主控供电从不中断，复位与烧录过程不会丢失"开机"状态。

### MCU 页——引脚分配

下表即固件依赖的分配（`components/board/include/espaperplay_config.h`
保持同步）：

| 功能 | GPIO | 模组脚 | 网络 | 对端 |
|---|---|---|---|---|
| BOOT 按键 | IO0 | 27 | BOOT_BTN | SW2 + R28 10K |
| 电池电压（ADC1_CH0） | IO1 | 39 | BAT_SENSE | R23/R24 分压 + C24 |
| 触摸复位 | IO2 | 38 | RST | FPC2.3 + R5 10K 上拉 |
| 触摸中断 | IO3 | 15 | INT | FPC2.4 |
| 触摸 I²C 数据 | IO4 | 4 | SDA | FPC2.5 + R3 10K |
| 触摸 I²C 时钟 | IO5 | 5 | SCL | FPC2.6 + R4 10K |
| VBUS 在位 | IO6 | 6 | USB_PRES | R25/R26 分压 |
| EPD 忙 | IO7 | 7 | BUSY | FPC1.9 |
| EPD 复位 | IO8 | 12 | RES | FPC1.10 |
| EPD 数据/命令 | IO9 | 17 | D/C | FPC1.11 |
| EPD 片选 | IO10 | 18 | CS | FPC1.12 |
| EPD SPI MOSI | IO11 | 19 | SDI | FPC1.14 |
| EPD SPI 时钟 | IO12 | 20 | SCLK | FPC1.13 |
| SD 时钟 | IO14 | 22 | SD_CLK | CARD1.5 |
| SD 命令 | IO15 | 8 | SD_CMD | CARD1.3 + R8 10K |
| SD 数据 0 | IO16 | 9 | SD_D0 | CARD1.7 + R9 10K |
| SD 数据 1 | IO17 | 10 | SD_D1 | CARD1.8 + R10 10K |
| SD 数据 2 | IO18 | 11 | SD_D2 | CARD1.1 + R6 10K |
| USB D− | IO19 | 13 | USB_DM | D5.4 → USB1 |
| USB D+ | IO20 | 14 | USB_DP | D5.6 → USB1 |
| SD 数据 3 | IO21 | 23 | SD_D3 | CARD1.2 + R7 10K |
| 充电中指示 | IO39 | 32 | CHRG_STAT | U1.7（TP4056 CHRG#） |
| 充满指示 | IO40 | 33 | STANDBY_STAT | U1.6（TP4056 STDBY#） |
| 外设轨使能 | IO41 | 34 | POWER_EN | R22 → Q2 栅极 |

- **EN**（3 脚）——R27 10K 上拉 + C27 1µF + SW1（复位）。
- **有意悬空**——IO35/36/37（28–30 脚）属于模组内八线 PSRAM，必须悬空；
  RXD0/TXD0（36/37 脚）预留给 UART0 调试；IO13/38/42/45/46/47/48 为空闲。
- 模组供电：GND（1/40/41）、3V3（2）配 C25 10µF + C26 100nF 去耦。

### EPD 页

24P FPC 连接器 FPC1（华宇创 C53436608）——引脚序已对照 Good Display
GDEY075T7-T01 官方规格书与 CAD 图纸核验：

- 逻辑脚 9–14 与 BUSY / RES / D/C / CS / SCLK / SDI 一一对应。
- **BS1（8 脚）接 GND**，选择 4 线 SPI。
- VDDIO/VCI（15/16 脚）由 **3V3_PER** 供电；VDD（18 脚）对地 1µF；VPP
  （19 脚）与温度传感器脚 TSCL/TSDA（6/7 脚）悬空。
- 正/负栅极与 VCOM 轨由本页分立升压电路产生——L1 10µH（FHD252012S-100MT，
  C602018）自 3V3_PER 接入 Q1（SI1308EDL，C7603347），栅极由面板 GDR 输出
  （2 脚）驱动、RESE（3 脚）做电流检测（R2 0.47Ω）；MBR0530 肖特基
  （D1–D3）与泵电容产生 VGH（21 脚）/ VGL（23 脚）；VCOM（24 脚）与
  VSHR/VSH1/VSL（5/20/22 脚）各配去耦电容。

### TOUCH 页

6P FPC 连接器 FPC2（莱联 C55172961）——引脚序与官方 GDEY075T7-T01 CAD
图纸完全一致：**GND / VCC / RESET / INT / SDA / SCL**。VCC 挂 3V3_PER；
I²C 上拉（R3/R4，10K）在同一轨上；RST 配 10K 上拉（R5），主控引脚高阻时
GT911 也能正常出复位——保护控制器的 I²C 地址锁存（已知失效模式，由驱动
自愈流程处理，见[电源与可靠性](power.md)）。

### SD 页

自弹式 microSD 卡座 CARD1（首韩 C393941），标准引脚序，4 位 SDMMC 走
GPIO14–18 + 21。VDD 挂 3V3_PER（10µF + 100nF）；CMD 与 DAT0–DAT3 各配
10K 上拉至 3V3_PER（CLK 有意不加）；卡检测开关未使用。

### 验证状态

- 全工程严格模式 **DRC：0 错误**；全部网络双端闭合（无悬空、无重复位号，
  77 个器件全部经网表核对）。
- 跨页网络（EPD/TOUCH/SD 信号、USB 差对、POWER_EN、分压、充电状态）经
  网表提取验证闭合。
- FPC1/FPC2 引脚序对照 Good Display 官方文档核验；充电 / 降压 / 负载开关
  引脚对照数据手册核验。
- USB ESD 通道配对已对照 ST 引脚配置图核实（D5：I/O1 = 1/6 脚、I/O2 =
  3/4 脚）——D+/D− 直通无交叉。
- 固件引脚表同步于 `espaperplay_config.h`；八线 PSRAM 已启用
  （`sdkconfig.defaults`），保留脚未动。

**打板前**——剩余人工检查项：

1. TS24CA 按键引脚分组：万用表实测，不按时信号脚对其余三个接地脚应全部
   开路、按下导通（规格书未标注内部分组）。
