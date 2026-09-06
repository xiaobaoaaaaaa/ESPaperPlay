# Display: EPD Driver & Rendering Backend · 显示：电子纸驱动与渲染后端

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

<a id="en"></a>
## English

### EPD Driver (GDEY075T7-T01 / UC8179)

The e-paper driver uses a differential (NEW/OLD plane) architecture: the CDI
`N2OCP` bit makes the controller auto-copy the new plane into the old plane
after every refresh, so no software frame-tracking is needed and partial
updates work on any background. Four refresh modes are exposed through
`espaperplay_epd_refresh()`:

| Mode | Frame format | Notes | Measured (room temp) |
| ---- | ------------ | ----- | -------------------- |
| `FULL` | 1 bpp, 48000 B | OTP fast waveform (force temp 0x5A) | ~1.7 s |
| `PARTIAL` | 1 bpp window (x / width 8-aligned) | PTOUT after DRF (vendor order) | ~0.37 s (80x80), ~0.52 s (full window) |
| `GRAY4` | 2 bpp, 96000 B (0=white ... 3=black) | factory 4-gray waveform, full screen only | — |
| `FAST` | 1 bpp, 48000 B | same waveform as FULL (PLL has no effect on this panel) | ~1.7 s |

Timings are end-to-end `espaperplay_epd_refresh()` durations including
controller (re)initialization; consecutive refreshes in the same mode skip the
re-init, and the driver deep-sleeps the panel automatically when no refresh
arrives for `ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS` (default 90 s, 0 to
disable; refresh always re-initializes on wake). A boot self-test
(`ESPAPERPLAY_EPD_ENABLE_SELFTEST`, off by default) exercises every mode,
prints timings, clears the panel to white and sleeps.

### GUI Backend (RGB565 + Mode Converters)

The GUI service (`components/graphics/ui`) provides the rendering backend: a
single **RGB565 main framebuffer** (750 KB, PSRAM) is the render target (LVGL
renders directly into it with `LV_COLOR_DEPTH=16`), and the e-paper's
1bpp / 2bpp constraints are confined to a conversion stage at flush time:

| Mode | Conversion | Refresh |
| ---- | ---------- | ------- |
| Interactive (`BW`) | RGB -> 1bpp: fast threshold or Bayer 4x4 (default; pure black/white bit-exact, no artifacts) | dirty-area partial, ~0.37-0.52 s |
| High-definition (`GRAY4`) | RGB -> 2bpp: Floyd-Steinberg error diffusion (default) or Bayer 4x4 | full screen, ~2.5 s |

Dirty areas are clipped, 8-pixel aligned (X) and merged into one refresh per
frame. Refreshes run **asynchronously on an internal worker task**: flush()
only snapshots (converts) the dirty area and queues it, so renderers (LVGL)
are never blocked by the ~0.4-2.6 s panel update; up to one frame is queued
ahead and later frames merge into it. `espaperplay_gui_wait_idle()` provides
an optional sync point. Page-level switches (`espaperplay_gui_show_ui` /
`show_image`) only change the conversion path — the framebuffer is never
re-rendered — and the driver guarantees clean transitions (gray4 -> BW inverts
the old plane, so residual mid-grays are erased). A self-test
(`ESPAPERPLAY_GUI_ENABLE_SELFTEST`, off by default) exercises both paths and
prints conversion and worker timings.

**SPI transfer path**: the PSRAM-resident frame data is DMA-transferred to the
panel through a **persistent internal-RAM staging buffer** reserved at init
(the panel SPI does not reliably DMA from PSRAM — bounce buffers fragment the
internal heap and direct PSRAM DMA underflows under bus contention). Occasional
transfer failures are retried by the refresh worker with a short backoff.

Screen orientation defaults to **portrait (panel rotated 90° clockwise,
logical 480x800)**: rotation is applied pixel-wise inside the LVGL flush
callback (`lv_display_set_rotation` semantics — logical dirty areas are
mapped to physical ones via `lv_display_rotate_area`, touch coordinates are
rotated by the LVGL core), so the rest of the stack stays panel-native.
The test screen's double-click cycles through all four orientations; every
screen scales its layout to the current logical resolution (portrait and
landscape).

Launcher app icons are LVGL A8 bitmaps generated from
[Iconify](https://icon-sets.iconify.design/) SVGs (mdi set) by
`tools/fonttools-venv/bin/python tools/prepare_icons.py` (downloads the SVG,
rasterizes with ImageMagick, emits `components/graphics/ui/src/icons_data.c`
+ `include/icons_data.h`); cards render icon + FreeType label below.

Weather support includes the official **QWeather icon set**
([icons.qweather.com](https://icons.qweather.com/), package
`QWeather-Icons-1.8.0/`, MIT): `tools/fonttools-venv/bin/python
tools/prepare_qweather_icons.py` converts the API icon codes (day/night,
rain/snow/fog variants) into LVGL A8 bitmaps
(`components/graphics/ui/src/qweather_icons.c`, lookup
`qweather_icon_get(code)`). The weather app icon on the home screen shows the
**live weather icon** from the latest snapshot (falls back to the mdi icon
when weather is unavailable or the code is unknown).

---

<a id="zh"></a>
## 简体中文

### EPD 驱动（GDEY075T7-T01 / UC8179）

电子纸驱动采用差分（新旧平面）架构：CDI 的 `N2OCP` 位使控制器在每次
刷新完成后自动把新平面拷贝到旧平面，无需软件维护"上一帧"，局部刷新可
在任意背景上工作。`espaperplay_epd_refresh()` 提供四种刷新模式：

| 模式 | 帧格式 | 说明 | 实测（室温） |
| ---- | ------ | ---- | ------------ |
| `FULL` | 1bpp 48000B | OTP 快刷波形（强制温度 0x5A） | ~1.7s |
| `PARTIAL` | 1bpp 窗口（x/width 8 对齐） | PTOUT 在 DRF 后（厂商顺序） | ~0.37s（80x80）/ ~0.52s（全屏窗口） |
| `GRAY4` | 2bpp 96000B（0=白…3=黑） | 出厂四灰阶波形，仅全屏 | — |
| `FAST` | 1bpp 48000B | 与 FULL 同波形（PLL 对本面板无效） | ~1.7s |

耗时为 `espaperplay_epd_refresh()` 端到端时长（含控制器初始化）；同模式
连续刷新会跳过重复初始化；驱动在最后一次刷新后超过
`ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS`（默认 90s，0 关闭）无新刷新时
自动深度睡眠（保底，刷新会自动唤醒）。上电自检
（`ESPAPERPLAY_EPD_ENABLE_SELFTEST`，默认关闭）可复验各模式并打印耗时。

### GUI 渲染后端（RGB565 + 模式转换级）

GUI 服务（`components/graphics/ui`）提供渲染后端：单一 **RGB565 主帧缓冲**
（750KB，PSRAM）作为渲染目标（LVGL 以 `LV_COLOR_DEPTH=16` 直接画入），
电子纸的 1bpp/2bpp 限制全部收敛到 flush 时的转换级：

| 模式 | 转换 | 刷新 |
| ---- | ---- | ---- |
| 交互（`BW`） | RGB->1bpp：快速阈值 或 Bayer 4x4（默认；纯黑/纯白位精确，无伪影） | 脏区局部，~0.37-0.52s |
| 高清（`GRAY4`） | RGB->2bpp：Floyd-Steinberg 误差扩散（默认）或 Bayer 4x4 | 全屏，~2.5s |

脏区自动裁剪、X 方向 8 像素对齐并合并为一帧一次刷新。刷新**异步化**：
flush() 只把脏区快照转换后排队，真实刷新由内部 worker 任务执行（约
0.4-2.6s 的面板更新不阻塞渲染器/LVGL）；最多一帧排队，后续帧合并进待刷帧；
`espaperplay_gui_wait_idle()` 提供可选同步点。页面级切换
（`espaperplay_gui_show_ui` / `show_image`）只改变转换路径、不重渲染主帧；
驱动保证切换刷新干净（灰阶->黑白反相旧平面，清除中间灰残留）。自检
（`ESPAPERPLAY_GUI_ENABLE_SELFTEST`，默认关闭）覆盖两条路径并打印转换与
worker 刷新耗时。

**SPI 传输路径**：PSRAM 中的帧数据经**初始化时预留的持久内部 RAM DMA
暂存区**搬运到面板（面板 SPI 从 PSRAM 直接 DMA 不可靠——bounce 缓冲会
打碎内部堆，PSRAM 直读在总线争抢下 DMA TX underflow）；偶发传输失败由
刷新 worker 短退避重试兜底。

屏幕朝向**默认竖屏**（面板顺时针旋转 90°，逻辑分辨率 480x800）：旋转在
LVGL flush 回调内逐像素完成（语义与 `lv_display_set_rotation` 一致——逻辑
脏区经 `lv_display_rotate_area` 映射为物理脏区，触摸坐标由 LVGL 内核旋转），
其余软件栈保持面板原生方向。测试页双击可循环四种朝向；所有页面布局均按
当前逻辑分辨率缩放（竖屏 / 横屏及其他分辨率）。

桌面应用图标为 LVGL A8 位图，由
[Iconify](https://icon-sets.iconify.design/)（mdi 图标集）的 SVG 生成：
`tools/fonttools-venv/bin/python tools/prepare_icons.py`（下载 SVG →
ImageMagick 栅格化 → 输出 `components/graphics/ui/src/icons_data.c` +
`include/icons_data.h`）；卡片渲染为「图标 + FreeType 中文名」。

天气功能支持官方**和风天气图标集**
（[icons.qweather.com](https://icons.qweather.com/)，图标包
`QWeather-Icons-1.8.0/`，MIT 许可）：`tools/fonttools-venv/bin/python
tools/prepare_qweather_icons.py` 把 API 图标代码（昼夜、雨/雪/雾各类）
转为 LVGL A8 位图（`components/graphics/ui/src/qweather_icons.c`，查找函数
`qweather_icon_get(code)`）。主界面「天气」应用图标显示**实时天气图标**
（取自最新快照，天气不可用或代码未收录时回退 mdi 图标）。
