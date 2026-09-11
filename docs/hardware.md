# Hardware Design · 硬件设计

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

> Snapshot taken 2026-09-12 from the live EasyEDA Pro project *ESPaperPlay*
> (EasyEDA 3.2.149, easyeda-agent v1.3.0). The schematic is the source of
> truth; this page summarizes it and will be re-synced as pages land.
>
> 本文为 2026-09-12 从嘉立创 EDA Pro 工程 *ESPaperPlay* 活体快照（原理图是
> 唯一事实源，页面推进后本文随之更新）。

---

<a id="en"></a>
## English

### Overview

The board is a custom carrier for the ESPaperPlay e-paper player firmware
(target: **ESP32-S3**). Peripheral pages are complete; the MCU and power
pages are still to be drawn.

| Schematic page | Status | Contents |
| -------------- | ------ | -------- |
| `EPD` | ✅ complete | 7.5″ e-paper (GDEY075T7-T01 / UC8179) FPC + boost/charge-pump circuit |
| `TOUCH` | ✅ complete | GT911 capacitive touch FPC + I²C pull-ups + decoupling |
| `SD` | ✅ complete | microSD (push) socket, 4-bit SDMMC wiring with pull-ups |
| `MCU` | ⬜ empty | ESP32-S3 minimal system — to be drawn |
| `POWER` | 🔶 drawn, gate pending | Battery power page (see below) — full circuit drawn, layout re-fit into the A4 frame pending |

Verification state of the three completed pages: per-page design check passes
with **0 findings**; the official schematic DRC returns **0 fatal / 0 error**
(8 warnings per page without per-item detail — the identical count appears on
the empty pages, so they are frame-level noise; still listed for review).
`PCB1` exists but is empty; PCB work has not started.

### System Architecture

A single **3V3** rail (from the future POWER page) feeds the MCU, the e-paper
logic supply and the touch controller; GND is the only other shared rail.
Every board-to-board signal is a named net waiting on the MCU page:

- **EPD → SPI**: `SCLK`, `SDI`, `CS`, `D/C`, `RES`, `BUSY`
- **Touch → I²C**: `SDA`, `SCL`, plus `INT`, `RST`
- **SD → SDMMC 4-bit**: `SD_CLK`, `SD_CMD`, `SD_D0`…`SD_D3`

Net audit: 29 unique nets project-wide; `GND`/`3V3` are consistently named
across all three drawn pages. 8 nets are currently single-pin (the MCU-facing
signals above) — expected until the MCU page lands, but each must terminate on
an MCU pin (or a net port) before the schematic gate can pass.

Note carried over from the current dev board: **touch and EPD share the 3V3
rail**, so the EPD boost inrush and WiFi burst currents ride one rail — the
POWER page should budget for this (rail impedance / decoupling), and layout
should keep the boost loop away from the touch FPC.

### EPD Page (complete)

**Connector** — `FPC1`: 24-pin, 0.5 mm FPC socket (HYCW3D-05FPC24-200B,
LCSC C53436608) for the panel's FPC tail.

**Power train** (per the GoodDisplay reference design):

- `Q1` SI1308EDL (SOT-323, C7603347) — low-side boost switch: gate = `GDR`
  (driven by the panel controller), source = `RESE`, drain = switch node.
- `L1` 10 µH (FHD252012S-100MT, C602018) from `3V3` to the switch node.
- `R2` 0.47 Ω from `RESE` to GND — current-sense resistor the controller
  uses to set drive strength.
- `R1` 1 MΩ from `GDR` to GND — bleeds the gate so the boost stays off while
  the controller's GDR output is high-Z.

**Charge pumps** (all diodes MBR0530 Schottky, C55068180):

- `+V`: `D3` half-wave rectifies the switch node into `PREVGH`
  (reservoir `C4` 4.7 µF) → FPC pin 21.
- `−V`: flying cap `C2` 4.7 µF couples the switch node onto the pump node;
  `D2` clamps the pump node to GND, `D1` rectifies it into `PREVGL`
  (reservoir `C8` 4.7 µF) → FPC pin 23.

**Panel rail reservoirs** — FPC pins 5 (`C1` 4.7 µF), 18 (`C6` 1 µF),
20 (`C7` 1 µF), 22 (`C9` 1 µF) and `VCOM` pin 24 (`C10` 1 µF), each to GND.
Their net names are auto-generated (`$3N50`, `$3N63`, `$3N64`, `$3N67`) —
cosmetic rename candidates.

**3V3 decoupling** — `C3` 4.7 µF + `C5` 1 µF at the connector.

**Signal pin map**

| FPC pin | Net | FPC pin | Net |
| ------- | --- | ------- | --- |
| 2 | GDR | 12 | CS |
| 3 | RESE | 13 | SCLK |
| 8 / 17 / 25 / 26 | GND | 14 | SDI |
| 9 | BUSY | 15 / 16 | 3V3 |
| 10 | RES | 21 | PREVGH |
| 11 | D/C | 23 / 24 | PREVGL / VCOM |

**Floating pins**: FPC 1, 4, 6, 7, 19 are unconnected and have no no-connect
markers yet (open item).

### Touch Page (complete)

**Connector** — `FPC2`: 6-pin, 0.5 mm FPC (LAIL-FPC-CDX01-6P0.5-GW,
LCSC C55172961) for the GT911 touch tail:

| FPC pin | Net | FPC pin | Net |
| ------- | --- | ------- | --- |
| 1 / 7 / 8 | GND | 4 | INT |
| 2 | 3V3 | 5 | SDA |
| 3 | RST | 6 | SCL |

- `R3`, `R4` 10 kΩ — I²C pull-ups (SDA/SCL → 3V3). Final value: an earlier
  review proposed 4.7 kΩ, but 10 kΩ was confirmed as the design of record
  (decision 2026-09-12).
- `R5` 10 kΩ — `RST` pull-up to 3V3 (keeps the controller out of reset while
  the MCU pin is high-Z; part of the GT911 address-latch self-healing story).
- `INT` deliberately has **no** pull-up (per design decision 2026-09).
- Decoupling at the connector: `C11` 100 nF + `C12`/`C13` 4.7 µF.

### SD Card Page (complete)

**Connector** — `CARD1`: push-push microSD socket (TF PUSH, LCSC C393941),
wired for **SDMMC 4-bit**: `CLK` / `CMD` / `DAT0`…`DAT3` as `SD_CLK`,
`SD_CMD`, `SD_D0`…`SD_D3`.

- Pull-ups `R6`–`R10` 10 kΩ on CMD and all four data lines → 3V3 (CLK: none).
- Decoupling: `C15` 100 nF + `C14` 10 µF.
- The card-detect switch pin (`CD`) is unused and unmarked (open item).

### Bill of Materials

13 line items; full list from the live BOM export:

| Qty | Value | Parts | MPN | LCSC |
| --- | ----- | ----- | --- | ---- |
| 9 | 4.7 µF | C1–C4, C7–C9, C12, C13 | CL10A475KA8NQNC (Samsung) | C69335 |
| 3 | 1 µF | C5, C6, C10 | CC0603KRX5R8BB105 (YAGEO) | C14664 |
| 2 | 100 nF | C11, C15 | CL10B104KB8NNNC (Samsung) | C1591 |
| 1 | 10 µF | C14 | *(generic — no MPN)* | — |
| 1 | TF socket | CARD1 | TF PUSH (SHOU HAN) | C393941 |
| 3 | MBR0530 | D1–D3 | MBR0530 (R+O) | C55068180 |
| 1 | 24P FPC 0.5 mm | FPC1 | HYCW3D-05FPC24-200B (HOAUC) | C53436608 |
| 1 | 6P FPC 0.5 mm | FPC2 | LAIL-FPC-CDX01-6P0.5-GW (LAILAN) | C55172961 |
| 1 | 10 µH | L1 | FHD252012S-100MT (cjiang) | C602018 |
| 1 | MOSFET | Q1 | SI1308EDL (TECH PUBLIC) | C7603347 |
| 1 | 1 MΩ | R1 | *(generic)* | — |
| 1 | 0.47 Ω | R2 | *(generic)* | — |
| 8 | 10 kΩ | R3–R10 | *(generic)* | — |

All passives are 0603. Generic placeholders (`C14`, `R1`, `R2`, `R3`–`R10`)
still need an MPN/LCSC assignment before fab.

### Power Page (drawn 2026-09-12, gate pending)

Circuit per the agreed power proposal — **39 parts, six functional blocks**,
wired in the same style as the other pages (real wires inside blocks, net
flags for rails and MCU-facing signals):

- **TYPEC_IN** — `J1` 16P Type-C (CC1/CC2 5.1 kΩ Rd each via `R11`/`R12`),
  `U3` USBLC6-2SC6 ESD on VBUS + D±; `USB_DP`/`USB_DM` reserved for the MCU's
  native USB.
- **CHARGER** — `U1` TP4056 (PROG 2 kΩ → 580 mA), LEDs on VBUS (indicate
  while charging with system off), `R21`/`R22` 10 kΩ pull CHRG#/STDBY# to 3V3
  → `CHRG_STS`/`STDBY_STS` to MCU.
- **LOAD_SHARE** — `Q2` AO3401A (D→VBAT, S→VSYS), `D4` SS34 (VBUS→VSYS),
  `D5`+`R16` gate network: USB present isolates the battery; unplugged, the
  battery takes over.
- **SOFT_SWITCH** — SW1 power key, `R25`/`R26`/`R27` sense divider
  (`PWR_BTN_SNS`), `Q5`+`Q6` latch driven by `PWR_HOLD`, `R28` 1 MΩ + `C23`
  10 µF hold capacitor (RST-safe, ≥3 s = force-off), `R23`/`R24`/`C22`.
- **LDO_3V3** — `U2` XC6220D331 (1 A, CE driven at full VSYS via PWR_EN).
- **MONITOR** — `R17`/`R18` 1 MΩ VBAT divider → `VBAT_SNS` (ADC),
  `R19`/`R20` 100 kΩ VBUS detect → `VBUS_SNS`.

Six zone frames + per-block notes are on the page. The EasyEDA **native DRC
passes with 0 fatal / 0 error** (15 item-less warnings, same profile as the
blank pages). **The strict agent gate does not pass yet**: the whole layout
was drawn against a 2970×2100 canvas while the project's A4 frame is
1170×825 usable (all 39 parts sit out-of-frame), and `bridge-check` flags 15
collinear-merge warnings that the native DRC does not confirm. **Next step**:
re-fit the layout into the A4 frame (compact re-placement, re-run the wire
topology at the new coordinates), then re-run `sch gate --strict`.

MCU-facing nets to terminate on the MCU page: `USB_DP`, `USB_DM`,
`VBAT_SNS`, `VBUS_SNS`, `CHRG_STS`, `STDBY_STS`, `PWR_BTN_SNS`, `PWR_HOLD`.

### Open Items

1. **MCU page** — draw the ESP32-S3 minimal system (strapping, flash/PSRAM,
   USB, boot/UART) and terminate the 8 single-pin nets listed above.
2. **POWER page** — battery input, charging, 3V3 regulator; budget for EPD
   boost + WiFi bursts on the shared rail.
3. **NC markers** — FPC1 pins 1/4/6/7/19 and `CARD1.CD` float without
   no-connect flags (the schematic check counts them as floating pins).
4. **Generic BOM rows** — assign MPN/LCSC to C14, R1, R2, R3–R10.
5. **Cosmetics before the schematic gate** — no functional-zone frames or
   per-module circuit notes have been drawn on any page yet, and the four
   `$3Nxx` panel-rail nets could take descriptive names.

---

<a id="zh"></a>
## 简体中文

### 概览

这块板是 ESPaperPlay 电子纸阅读器固件（目标芯片 **ESP32-S3**）的自制底板。
外设页已完成，主控页与电源页尚未绘制。

| 原理图页 | 状态 | 内容 |
| -------- | ---- | ---- |
| `EPD` | ✅ 完成 | 7.5″ 电子纸（GDEY075T7-T01 / UC8179）FPC + 升压/电荷泵电路 |
| `TOUCH` | ✅ 完成 | GT911 电容触摸 FPC + I²C 上拉 + 去耦 |
| `SD` | ✅ 完成 | TF（自弹）卡座，SDMMC 4-bit 接法 + 上拉 |
| `MCU` | ⬜ 空页 | ESP32-S3 最小系统——待绘制 |
| `POWER` | 🔶 已画、门禁未过 | 电池供电页（见下）——电路全部画完，待整体收进 A4 图框 |

三个已完成页的验证状态：逐页设计检查 **0 findings** 通过；官方原理图 DRC
**0 fatal / 0 error**（每页 8 条无法细化到条目的 WARN——空白页同样是 8 条，
基本可断定为图框类噪音，仍列入待审阅）。`PCB1` 已存在但为空，PCB 尚未开始。

### 系统架构

单一 **3V3** 轨（由未来的 POWER 页产生）同时供给主控、电子纸逻辑电与触摸
控制器；GND 是唯一另一条共享轨。所有板内互连都是挂在主控页上的命名网络：

- **EPD → SPI**：`SCLK`、`SDI`、`CS`、`D/C`、`RES`、`BUSY`
- **触摸 → I²C**：`SDA`、`SCL`，另有 `INT`、`RST`
- **SD → SDMMC 4-bit**：`SD_CLK`、`SD_CMD`、`SD_D0`…`SD_D3`

网络审计：全工程 29 条独立网络；`GND`/`3V3` 三个已绘制页命名一致。当前
8 条单引脚网（即上述主控侧信号）——主控页落地前属预期，但原理图门禁通过
前每条都必须落到主控引脚（或网络端口）上。

从现役开发板带过来的注意事项：**触摸与电子纸共用 3V3 轨**，EPD 升压浪涌
与 WiFi 突发电流同轨叠加——POWER 页需按此做预算（轨阻抗/去耦），布局上
升压环路应远离触摸 FPC。

### EPD 页（完成）

**连接器**——`FPC1`：24P 0.5mm FPC 座（HYCW3D-05FPC24-200B，
LCSC C53436608），对插屏幕 FPC。

**功率链**（按 GoodDisplay 参考设计）：

- `Q1` SI1308EDL（SOT-323，C7603347）——低侧升压开关：栅 = `GDR`（屏内
  控制器驱动），源 = `RESE`，漏 = 开关节点。
- `L1` 10 µH（FHD252012S-100MT，C602018），`3V3` → 开关节点。
- `R2` 0.47 Ω，`RESE` → GND——屏控制器检测电流、设定驱动强度的采样电阻。
- `R1` 1 MΩ，`GDR` → GND——泄放电阻，控制器 GDR 高阻时保证开关管关断。

**电荷泵**（二极管均为 MBR0530 肖特基，C55068180）：

- `+V`：`D3` 对开关节点半波整流 → `PREVGH`（储能 `C4` 4.7 µF）→ FPC 21 脚。
- `−V`：飞跨电容 `C2` 4.7 µF 把开关节点耦合到泵节点；`D2` 把泵节点钳到
  GND，`D1` 整流进 `PREVGL`（储能 `C8` 4.7 µF）→ FPC 23 脚。

**屏侧轨储能**——FPC 5 脚（`C1` 4.7 µF）、18 脚（`C6` 1 µF）、
20 脚（`C7` 1 µF）、22 脚（`C9` 1 µF）与 `VCOM` 24 脚（`C10` 1 µF），
各自对 GND。这几条网络名是自动生成的（`$3N50` 等）——可改成语义名。

**3V3 去耦**——连接器旁 `C3` 4.7 µF + `C5` 1 µF。

**信号引脚表**

| FPC 脚 | 网络 | FPC 脚 | 网络 |
| ------ | ---- | ------ | ---- |
| 2 | GDR | 12 | CS |
| 3 | RESE | 13 | SCLK |
| 8 / 17 / 25 / 26 | GND | 14 | SDI |
| 9 | BUSY | 15 / 16 | 3V3 |
| 10 | RES | 21 | PREVGH |
| 11 | D/C | 23 / 24 | PREVGL / VCOM |

**悬空引脚**：FPC 1、4、6、7、19 未连接且尚未放非连接标记（待办）。

### 触摸页（完成）

**连接器**——`FPC2`：6P 0.5mm FPC（LAIL-FPC-CDX01-6P0.5-GW，
LCSC C55172961），对插 GT911 触摸排线：

| FPC 脚 | 网络 | FPC 脚 | 网络 |
| ------ | ---- | ------ | ---- |
| 1 / 7 / 8 | GND | 4 | INT |
| 2 | 3V3 | 5 | SDA |
| 3 | RST | 6 | SCL |

- `R3`、`R4` 10 kΩ——I²C 上拉（SDA/SCL → 3V3）。定案值：早期评审曾提议
  4.7 kΩ，2026-09-12 确认维持 10 kΩ 为最终设计。
- `R5` 10 kΩ——`RST` 上拉到 3V3（主控引脚高阻时保持控制器不进复位，
  属 GT911 地址锁存自愈方案的一部分）。
- `INT` 按设计决策（2026-09）**不上拉**。
- 连接器去耦：`C11` 100 nF + `C12`/`C13` 4.7 µF。

### SD 卡页（完成）

**连接器**——`CARD1`：自弹式 TF 卡座（TF PUSH，LCSC C393941），
按 **SDMMC 4-bit** 接线：`CLK`/`CMD`/`DAT0`…`DAT3` 对应
`SD_CLK`、`SD_CMD`、`SD_D0`…`SD_D3`。

- 上拉 `R6`–`R10` 10 kΩ：CMD 与四根数据线全部上拉到 3V3（CLK 不上拉）。
- 去耦：`C15` 100 nF + `C14` 10 µF。
- 卡检测开关脚（`CD`）未使用且未标记（待办）。

### 物料清单

共 13 行；以下为活体 BOM 导出结果：

| 数量 | 值 | 器件 | 型号 | LCSC |
| ---- | -- | ---- | ---- | ---- |
| 9 | 4.7 µF | C1–C4, C7–C9, C12, C13 | CL10A475KA8NQNC（三星） | C69335 |
| 3 | 1 µF | C5, C6, C10 | CC0603KRX5R8BB105（国巨） | C14664 |
| 2 | 100 nF | C11, C15 | CL10B104KB8NNNC（三星） | C1591 |
| 1 | 10 µF | C14 | *（通用件，无型号）* | — |
| 1 | TF 卡座 | CARD1 | TF PUSH（首韩） | C393941 |
| 3 | MBR0530 | D1–D3 | MBR0530（宏嘉诚） | C55068180 |
| 1 | 24P FPC 0.5mm | FPC1 | HYCW3D-05FPC24-200B（华宇创） | C53436608 |
| 1 | 6P FPC 0.5mm | FPC2 | LAIL-FPC-CDX01-6P0.5-GW（莱联） | C55172961 |
| 1 | 10 µH | L1 | FHD252012S-100MT（长江微电） | C602018 |
| 1 | MOS 管 | Q1 | SI1308EDL（台舟） | C7603347 |
| 1 | 1 MΩ | R1 | *（通用件）* | — |
| 1 | 0.47 Ω | R2 | *（通用件）* | — |
| 8 | 10 kΩ | R3–R10 | *（通用件）* | — |

阻容全部 0603。通用占位件（`C14`、`R1`、`R2`、`R3`–`R10`）打板前需补
型号/LCSC 编号。

### 电源页（2026-09-12 已画，门禁未过）

按定稿方案落地——**39 器件、六个功能块**，画法与其余三页一致（块内真导线、
电源轨与 MCU 信号用网络旗标）：

- **TYPEC_IN**——`J1` 16P Type-C（CC1/CC2 各 5.1 kΩ Rd，`R11`/`R12`）、
  `U3` USBLC6-2SC6 护 VBUS+D±；`USB_DP`/`USB_DM` 预留 MCU 原生 USB。
- **CHARGER**——`U1` TP4056（PROG 2 kΩ→580 mA），LED 挂 VBUS（关机充电仍指示），
  `R21`/`R22` 10 kΩ 把 CHRG#/STDBY# 上拉到 3V3→`CHRG_STS`/`STDBY_STS` 回读。
- **LOAD_SHARE**——`Q2` AO3401A（D→VBAT、S→VSYS）、`D4` SS34（VBUS→VSYS）、
  `D5`+`R16` 栅极网络：USB 在时电池隔离、拔出后电池接管。
- **SOFT_SWITCH**——SW1 电源键、`R25`/`R26`/`R27` 分压回读（`PWR_BTN_SNS`）、
  `Q5`+`Q6` 锁存（固件 `PWR_HOLD` 保持）、`R28` 1 MΩ+`C23` 10 µF 保持电容
  （RST 复位不掉电、≥3 s 强制断电）、`R23`/`R24`/`C22`。
- **LDO_3V3**——`U2` XC6220D331（1 A，CE 由 PWR_EN 全压驱动）。
- **MONITOR**——`R17`/`R18` 1 MΩ 电池分压→`VBAT_SNS`（ADC）、
  `R19`/`R20` 100 kΩ VBUS 检测→`VBUS_SNS`。

分区框与逐块电路说明已放。EasyEDA **原生 DRC 0 fatal / 0 error**（15 条无明细
WARN，与空白页同型）。**agent 严格门禁尚未通过**：整版按 2970×2100 画布布的，
而工程 A4 图框可用区只有 1170×825（39 件全部在框外），另有 15 条共线合并类
警告（原生 DRC 未证实）。**下一步**：把布局整体收进 A4 框（紧凑重排 + 按新
坐标重放连线拓扑），再跑 `sch gate --strict`。

待 MCU 页接入的网络：`USB_DP`、`USB_DM`、`VBAT_SNS`、`VBUS_SNS`、`CHRG_STS`、
`STDBY_STS`、`PWR_BTN_SNS`、`PWR_HOLD`。

### 待办与风险

1. **主控页**——绘制 ESP32-S3 最小系统（strapping、flash/PSRAM、USB、
   BOOT/串口），并把上列 8 条单引脚网接到引脚上。
2. **电源页**——电池输入、充电、3V3 稳压；按共轨上的 EPD 升压浪涌 +
   WiFi 突发电流做预算。
3. **非连接标记**——FPC1 的 1/4/6/7/19 脚与 `CARD1.CD` 悬空无 NC 标记
   （设计检查把它们计为悬空引脚）。
4. **通用 BOM 行**——给 C14、R1、R2、R3–R10 补型号/LCSC 编号。
5. **门禁前整饰**——各页尚未画功能区框与逐模块电路说明；四条 `$3Nxx`
   屏侧轨网络可改成语义名。
