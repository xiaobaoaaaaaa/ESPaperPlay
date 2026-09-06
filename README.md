# ESPaperPlay

> An open-source e-paper device development platform built on **ESP32-S3**.
>
> 基于 **ESP32-S3** 的开源电子纸设备开发平台。

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

<a id="en"></a>
## English

ESPaperPlay is an embedded software platform for low-power e-paper applications. The first milestone — a low-power e-reader built around an **ESP32-S3 + 7.5-inch e-ink display** — now works end to end: TXT/EPUB reading, a weather station screen, on-device file management, a first-boot setup wizard, an HTTPS Web management console, and an activity-aware auto-light-sleep power system. The architecture keeps clear extension points for desktop info panels, smart calendars, Home Assistant status screens, e-labels, and more.

The repository is under active development with a modular, maintainable, and extensible architecture. See [CHANGELOG.md](CHANGELOG.md) for release history.

### Feature Overview

- **E-paper driver** — full / partial / 4-gray / fast refresh modes, async refresh worker, anti-ghosting (see the [Display](docs/display.md) doc)
- **Launcher desktop (LVGL 9)** — status bar, clock page, app cards (Weather / Reader / Files / Settings), page-stack navigation, gesture + physical key input, multi-resolution layouts (portrait / landscape)
- **E-reader** — TXT + EPUB 2/3: table of contents, page-number jump, font sizes, image pages with automatic gray-scale refresh, reading history & bookshelf, SD-backed pagination caches for instant re-opens
- **Weather station screen** — current conditions, 24 h temperature curve, 7-day dual-curve forecast, weather alerts, air quality & life indices, sun/moon arcs with the real moon-phase icon
- **File manager** — on-device browsing (TXT/EPUB open straight into the reader) + full Web file management (upload / download / mkdir / rename / delete)
- **Web management console (HTTPS)** — settings, WiFi scanning, font upload & selection, SD file manager, first-boot wizard, remote touch diagnostics, factory reset
- **Low power** — activity-aware auto light sleep, minute-aligned timer wakeup (weather refreshes even while asleep), software RTC drift calibration without an external 32k crystal
- **Reliability** — GT911 touch self-healing (auto recovery from the address-flip failure mode), SD diagnostic logging with a boot/sleep lifecycle timeline

### Hardware Platform

| Module  | Model                   | Description                                    |
| ------- | ----------------------- | ---------------------------------------------- |
| MCU     | ESP32-S3-WROOM-1-N16R8  | 16MB Flash, 8MB PSRAM                         |
| Display | Good Display GDEY075T7-T01 | 7.5", 800x480, UC8179 controller, SPI      |
| Touch   | GT911                   | Capacitive touch, I2C + INT + RESET            |
| Storage | MicroSD  | SDIO (SDMMC 4-bit), EPUB / TXT / images / config files |
| Power   | Low-power design        | Activity-aware auto light sleep, timer wakeup, software RTC drift calibration |

Default GPIO and bus parameters are defined centrally in
`components/board/include/espaperplay_config.h`.

### Documentation

Detailed design & implementation notes live in [docs/](docs/) (each bilingual):

| Document | Contents |
| -------- | -------- |
| [Architecture](docs/architecture.md) | Directory structure, layered dependencies, naming & logging conventions |
| [Display](docs/display.md) | EPD driver (4 refresh modes), RGB565 rendering backend, icons |
| [Fonts](docs/fonts.md) | `fonts` partition assets, SD full font, FreeType rendering pipeline |
| [UI & Input](docs/ui.md) | Launcher screens, input events (keys / touch), device settings page |
| [Storage](docs/storage.md) | MicroSD SDIO + FATFS stack, on-device & web file manager |
| [Network Time](docs/network-time.md) | Public IP → geolocation → timezone → NTP |
| [Weather](docs/weather.md) | QWeather service and the weather screen |
| [Reader](docs/reader.md) | TXT/EPUB parsing, block model, pagination, images |
| [Power & Reliability](docs/power.md) | Auto light sleep, GT911 touch self-healing, SD diagnostic log |
| [Web Console](docs/web-console.md) | HTTPS console and full API reference |

### Building

#### Requirements

- ESP-IDF **v6.1.0** (target chip: ESP32-S3)
- CMake ≥ 3.22
- Third-party code lives in the ESP Component Registry managed dependencies;
  the only in-tree vendor copy is TJpgDec (`components/applications/reader/src/libs/tjpgd`,
  kept outside LVGL so `LV_USE_TJPGD` cannot compile it away).

#### Build

```bash
# 1. Load the ESP-IDF environment
. $HOME/esp/esp-idf/export.sh        # adjust the path to your installation

# 2. Set the target chip (first time or when switching chips)
idf.py set-target esp32s3

# 3. Build
idf.py build

# 4. Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

`idf.py flash` flashes the app **and** the `fonts` partition image (built from
`assets/fonts/`, see the [Fonts doc](docs/fonts.md)). For day-to-day iteration use
**`idf.py app-flash`** (app only, no font partition) — run a full `flash` only
after the first clone or whenever `assets/fonts/` changes.

On a fresh clone, `sdkconfig.defaults` (16MB Flash / 8MB PSRAM) is applied automatically; adjust via `idf.py menuconfig` if needed. The defaults enable `-O2` compilation and CPU dynamic frequency scaling (DFS: up to 240 MHz when busy, 80 MHz when idle) to balance performance and power.

#### CI / Releases

GitHub Actions (`.github/workflows/build.yml`) builds with the
`espressif/idf:v6.1` container on every push to `main`, every `v*` tag and
every PR, and uploads the **full flash image set** (bootloader / partition
table / app / fonts partition / flash_args) as artifacts.

Pushing a `v*` tag on `main` additionally creates a **GitHub Release**: the
workflow packages the full firmware into `<tag>-flash.tar.gz` (with a
`FLASH.txt` flashing guide and `SHA256SUMS`), and fills the release notes from
the matching version section of [CHANGELOG.md](CHANGELOG.md).

### Roadmap

- [x] **Phase 0**: software architecture bootstrap, module skeletons, build system & CI
- [x] **Phase 1**: Board drivers (GPIO / SPI / I2C bus init)
- [x] **Phase 2**: EPD (UC8179) driver & refresh (full / partial / gray4 / fast), GT911 touch driver
- [x] **Phase 3**: Low-power management (activity-aware auto light sleep, timer wakeup, RTC drift calibration)
- [x] **Phase 4**: SD card + FATFS file system (SDIO/SDMMC)
- [x] **Phase 5**: LVGL UI framework (launcher desktop, pages, settings, first-boot wizard, weather screen)
- [x] **Phase 6**: Reader — TXT + EPUB (TOC / pagination caches / images); PDF not planned short-term
- [ ] **Phase 7**: Networking & IoT features (optional)
  - [x] Web management console (HTTPS): status / settings / files / fonts / wizard / touch diagnostics
  - [x] Net time sync: public IP → geolocation → timezone → NTP (uapis.cn)
  - [ ] OTA firmware update (partition table is factory-only today)

### License

This project is open-sourced under the [MIT License](LICENSE).

---

<a id="zh"></a>
## 简体中文

ESPaperPlay 是一个面向低功耗电子纸应用的嵌入式软件平台。第一阶段目标——
基于 **ESP32-S3 + 7.5 英寸电子墨水屏** 的低功耗阅读器——已端到端可用：
TXT/EPUB 阅读、天气信息屏、设备端文件管理、首次开机引导向导、HTTPS Web
管理控制台，以及基于活动感知的自动浅睡眠低功耗体系。架构上仍为桌面
信息屏、智能日历、Home Assistant 状态屏、电子标签等应用预留清晰的扩展点。

当前仓库处于**积极开发阶段**，模块化、可维护、可扩展。发布历史见
[CHANGELOG.md](CHANGELOG.md)。

### 功能总览

- **电子纸驱动**——全刷 / 局刷 / 四灰 / 快刷四种模式、异步刷新 worker、残影治理（详见 [显示文档](docs/display.md)）
- **主界面桌面（LVGL 9）**——状态栏、时钟页、应用卡片（天气 / 阅读器 / 文件 / 设置）、
  页面栈导航、手势 + 物理按键输入、多分辨率布局（竖屏 / 横屏自适应）
- **阅读器**——TXT + EPUB 2/3：目录跳转、页码跳转、字号档位、插图页自动灰度刷新、
  阅读历史与书架、SD 分页缓存秒开
- **天气信息屏**——实时条件、24h 气温曲线、7 天双曲线预报、气象预警、
  空气质量与生活指数、日出月落弧线（月亮按真实月相显示）
- **文件管理**——设备端浏览（TXT/EPUB 直接进阅读器）+ Web 端完整管理
 （上传 / 下载 / 新建目录 / 重命名 / 删除）
- **Web 管理控制台（HTTPS）**——设置、WiFi 扫描、字体上传与选用、SD 文件管理、
  首次开机向导、远程触摸诊断、恢复出厂
- **低功耗**——活动感知的自动浅睡眠、分钟对齐定时唤醒（睡眠中天气照常刷新）、
  无外部 32k 晶振的软件 RTC 漂移标定
- **可靠性**——GT911 触摸自愈（地址翻转失效自动恢复）、SD 诊断日志与
  开机 / 睡眠生命周期时间线

### 硬件平台

| 模块        | 型号                 | 说明                                    |
| ----------- | -------------------- | --------------------------------------- |
| MCU         | ESP32-S3-WROOM-1-N16R8 | Flash 16MB，PSRAM 8MB                  |
| 显示屏      | 佳显 GDEY075T7-T01   | 7.5"，800x480，UC8179 控制器，SPI 接口  |
| 触摸屏      | GT911                | 电容触摸，I2C + INT + RESET             |
| 存储        | MicroSD              | SDIO（SDMMC 4-bit），EPUB / TXT / 图片 / 配置文件 |
| 电源        | 低功耗设计            | 活动感知自动浅睡眠、定时唤醒、软件 RTC 漂移标定 |

GPIO 与总线默认参数统一在 `components/board/include/espaperplay_config.h`
中定义。

### 文档

详细设计与实现说明见 [docs/](docs/)（每篇均含中英双语）：

| 文档 | 内容 |
| ---- | ---- |
| [软件架构](docs/architecture.md) | 目录结构、分层依赖、命名与日志规范 |
| [显示](docs/display.md) | EPD 驱动（四种刷新模式）、RGB565 渲染后端、图标生成 |
| [字体](docs/fonts.md) | `fonts` 分区资产、SD 完整字库、FreeType 渲染链路 |
| [界面与输入](docs/ui.md) | 桌面页面、输入事件（按键 / 触摸）、设备端设置页 |
| [存储](docs/storage.md) | MicroSD SDIO + FATFS 栈、设备端与 Web 文件管理 |
| [网络时间](docs/network-time.md) | 公网 IP → 地理位置 → 时区 → NTP |
| [天气](docs/weather.md) | 和风天气服务与天气页 |
| [阅读器](docs/reader.md) | TXT/EPUB 解析、块模型、分页、图片 |
| [电源与可靠性](docs/power.md) | 自动浅睡眠、GT911 触摸自愈、SD 诊断日志 |
| [Web 管理控制台](docs/web-console.md) | HTTPS 控制台与完整 API 列表 |

### 编译方法

#### 环境要求

- ESP-IDF **v6.1.0**（目标芯片 ESP32-S3）
- CMake ≥ 3.22
- 第三方代码均在 ESP 组件注册表托管依赖中；仓库内唯一的 vendor 副本是
  TJpgDec（`components/applications/reader/src/libs/tjpgd`，置于 LVGL 之外，
  避免 `LV_USE_TJPGD` 把它裁空）。

#### 编译

```bash
# 1. 加载 ESP-IDF 环境
. $HOME/esp/esp-idf/export.sh        # 路径以实际安装为准

# 2. 设置目标芯片（首次或切换芯片时）
idf.py set-target esp32s3

# 3. 编译
idf.py build

# 4. 烧录并查看日志
idf.py -p /dev/ttyUSB0 flash monitor
```

`idf.py flash` 会同时烧录 app **和** `fonts` 字体分区镜像（由
`assets/fonts/` 构建，见 [字体文档](docs/fonts.md)）。日常迭代建议用
**`idf.py app-flash`**（只烧 app，不烧字体分区）；首次克隆或
`assets/fonts/` 内容变更后再执行完整 `flash`。

首次克隆时，`sdkconfig.defaults`（Flash 16MB / PSRAM 8MB）会自动生效；
如需调整请使用 `idf.py menuconfig`。默认配置已启用 `-O2` 编译优化与 CPU
动态调频（DFS：繁忙升至 240MHz，空闲降至 80MHz），平衡性能与功耗。

#### CI / 发布

GitHub Actions（`.github/workflows/build.yml`）在每次推送到 `main`、打
`v*` tag 以及每个 PR 时用 `espressif/idf:v6.1` 容器构建，并上传**完整
烧录镜像组**（bootloader / 分区表 / APP / fonts 分区 / flash_args）为
Artifacts。

在 `main` 上打 `v*` tag 会进一步自动创建 **GitHub Release**：工作流把完整
固件打包为 `<tag>-flash.tar.gz`（附 `FLASH.txt` 烧录说明与 `SHA256SUMS`
校验和），Release 正文自动取自 [CHANGELOG.md](CHANGELOG.md) 中对应版本段落。

### 开发路线

- [x] **Phase 0**：软件架构初始化、模块骨架、构建系统与 CI
- [x] **Phase 1**：Board 驱动（GPIO / SPI / I2C 总线初始化）
- [x] **Phase 2**：EPD（UC8179）驱动与刷新（全刷/局刷/灰阶/快刷）、GT911 触摸驱动
- [x] **Phase 3**：低功耗管理（活动感知自动浅睡眠、定时唤醒、RTC 漂移标定）
- [x] **Phase 4**：SD 卡 + FATFS 文件系统（SDIO/SDMMC 接口）
- [x] **Phase 5**：LVGL 界面框架（主界面桌面、页面栈、设置页、首次开机向导、天气页）
- [x] **Phase 6**：阅读器——TXT + EPUB（目录 / 分页缓存 / 图片）；PDF 短期无计划
- [ ] **Phase 7**：网络与物联网功能（可选）
  - [x] Web 管理控制台（HTTPS）：状态 / 设置 / 文件 / 字体 / 向导 / 触摸诊断
  - [x] 网络时间同步：公网 IP → 地理位置 → 时区 → NTP（uapis.cn）
  - [ ] OTA 固件升级（当前分区为 factory 单区，无 A/B）

### License

本项目基于 [MIT License](LICENSE) 开源。
