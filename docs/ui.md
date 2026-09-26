# UI, Screens & Input · 界面、页面与输入

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

<a id="en"></a>
## English

### Launcher UI & Screens

- **Boot log screen** (`screen_boot`): the panel lights up immediately and
  shows initialization progress while the rest of the system (SD, touch,
  network…) initializes **in parallel on a second thread** — the display is
  never blocked by slow peripherals at boot.
- **First-boot wizard** (`screen_setup`): on an unconfigured device a guided
  setup runs before the desktop — configure WiFi (scan list or manual entry)
  either on-device or from the Web console via QR code; factory reset is
  available from the Web console.
- **Home desktop** (`screen_home`): centered clock area (horizontal hour/minute digits,
  weekday and date) above the app grid (**weather / reader / files / settings**).
  Tapping a card pushes the application page; the weather card shows the live
  QWeather icon.
- **Sleep screensaver** (`screen_screensaver`): pushed when the device auto
  sleeps while on the home screen — a large clock, date, weather summary and
  version footer that the e-paper bistable panel keeps showing throughout
  light sleep. Minute-aligned timer wakeups refresh the clock inside the wake
  window; any user wake (touch/key) pops the screensaver and rebuilds the home
  screen.
- **Unified status bar**: time, WiFi state and the sleep/energy icon live on
  every screen and share a single refresh path (except the screensaver).
- Page-stack navigation with edge-swipe back (24 px trigger width) and the
  physical BOOT key; the application layouts target the device
  portrait resolution (480x800). Landscape layouts require separate visual validation.

### Input: Physical Keys & Event Queues

The input service (`components/services/input`) aggregates all human input
sources (physical keys, touch) into a unified event stream. The LVGL thread
consumes each queue directly via `espaperplay_input_try_get_key()` /
`espaperplay_input_try_get_touch()`.

**Physical keys** — BOOT (GPIO0), volume up (GPIO2) and volume down (GPIO5),
all active low, are driven by the official
[`espressif/button`](https://components.espressif.com/components/espressif/button)
component (v4.2.0, managed dependency). Raw driver events are normalized into
`espaperplay_input_key_action_t`: PRESS_DOWN / PRESS_UP / SINGLE_CLICK /
DOUBLE_CLICK / LONG_PRESS_START / LONG_PRESS_HOLD / LONG_PRESS_UP (with press
duration). `LONG_PRESS_HOLD` is throttled per key to one event per 500 ms — the driver
default fires every 20 ms, which would flood the event pipeline and make
`LONG_PRESS_UP` get dropped.

**Dual queues (key / touch isolated)** — two independent FreeRTOS queues:

| Queue | Depth | Policy |
| ----- | ----- | ------ |
| Key | 16 | `xQueueSendToFront` + evict-oldest when full — key events are never lost |
| Touch | 32 | `xQueueSend` drop-newest when full — intermediate points for trajectory drawing are preserved |

Touch is interrupt-driven (GT911 INT wakes a task — I2C cannot run in an ISR —
which reads coordinates and posts them to the touch queue), so high-rate touch
traffic can never crowd out key events. The LVGL thread drains the touch queue
every indev read period (~30 ms), so backlog stays bounded.

**GUI input consumption** — both queues are read inside the LVGL thread
(`espaperplay_ui_input_start()`), no dispatcher task and no cross-thread
posting:

- **Volume keys** change the global volume by 10% on click/hold, persist to NVS
  and update the audio software gain; they are not routed to page navigation.
- **Keys** are drained by a key-pump `lv_timer` (20 ms period) and routed
  one-by-one to the top page's optional `on_key` hook (extended
  `espaperplay_ui_page_t`). Navigation decisions belong to pages.
- **Default long-press action** — the BOOT key's `LONG_PRESS_START` is caught
  globally (in the page-stack key funnel, before page hooks) and triggers a
  configurable action **once per press** (HOLD / release events never re-fire):
  `full_refresh` (default, forces a full-screen deep refresh to clear ghosting),
  `back` (pop the current page), or `none`. It applies to every page and is
  configurable from the device settings page or the Web console
  (`boot_long_press_action`, NVS-persisted, effective immediately).
  The long-press trigger time itself is also configurable
  (`boot_long_press_time_ms`, default 1000 ms, range 300-10000 ms, applied to
  the button driver at boot and re-applied live).
- **Touch** is drained by the pointer indev's `read_cb` (see `lvgl_touch.c`):
  every queued event is forwarded to the top page's optional `on_touch` hook
  (every intermediate coordinate survives for trajectory drawing), and the
  press state machine drives LVGL widgets. A click-latch FIFO replays
  press+release pairs that arrived entirely while the LVGL thread was busy
  rendering, so rapid taps are never swallowed.

- Home app cards launch the applications (Weather / Reader / Files /
  Settings);
- Test page (partial-refresh stress test + live key/touch display, entered
  from **Settings → 系统 → 测试页**): a touch pad draws finger trajectories
  as connected polylines (8 strokes × 512 points, `clear` button empties the
  pad, `back` button pops the page through the LVGL pointer indev);
  long-press release → pop back.

A boot self-test (`ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST`, off by default) injects
synthetic key events through the real input queue and asserts the page-stack
response (1→2→1→2), printing `key selftest PASS/FAIL`.

### Device Settings Page

`screen_settings` (home → 设置) is the on-device control panel:

- A category landing page opens network, display, keys, reader, weather and
  system details. Back returns to the category list before leaving settings.
- Whole-row tap targets; numeric items (idle-sleep timeout, force-full-refresh
  threshold, BOOT long-press time) open a `[-] [+]` stepper modal; every
  value is applied and persisted only after Save; Cancel discards edits.
- BOOT key long-press action (full refresh / back / none) and WiFi mode (AP /
  STA, with confirmation) cycle on tap.
- **WiFi networks** opens the scan list page (`screen_wifi_list`): signal
  strength + encryption icons, current network highlighted, password keyboard
  for secured networks, cancellable connect view; AP mode recovers the
  soft AP on cancel/timeout so the Web console stays reachable.
- Items awkward to type on an e-ink screen (WiFi credentials, QWeather API
  key / location / API host) display their status and point to the Web
  console.
- Font selection (built-in + SD fonts), firmware version row, and a developer
  group (test page entry).

---

<a id="zh"></a>
## 简体中文

### 主界面与页面

- **开机日志屏**（`screen_boot`）：面板即刻点亮并显示初始化进度，系统其余
  部分（SD / 触摸 / 网络等）**在第二个线程上并行初始化**——开机点亮不再被
  慢外设阻塞。
- **首次开机引导**（`screen_setup`）：未配置的设备先进引导再进桌面——
  配置 WiFi（扫描列表选择或手动输入），支持设备端本机配置与 Web 端扫码
  两种方式；恢复出厂在 Web 管理页。
- **主界面桌面**（`screen_home`）：居中时钟区（横排大字时/分 + 星期 + 日期）+
  应用卡片（**天气 / 阅读器 / 文件 / 设置**）。点击卡片进入应用；天气
  卡片显示和风实时天气图标。
- **睡眠屏保页**（`screen_screensaver`）：自动浅睡眠且位于主界面时压入，
  大字时钟 + 日期星期 + 天气摘要 + 版本页脚在睡眠期间由电子纸双稳态
  保持；定时器唤醒借刷新窗口更新时钟，用户唤醒（触摸/按键）即弹出、
  重建主界面。
- **统一状态栏**：时间、WiFi 状态、睡眠（节能）图标出现在所有页面，
  共用同一条刷新路径（屏保页除外）。
- 页面栈导航 + 边缘滑动返回（24px 触发宽度）+ 手势翻页 + 物理按键；
  应用布局以设备竖屏 480x800 为目标，横屏布局仍需单独验证。

### 输入：物理按键与事件队列

输入服务（`components/services/input`）把人机输入源（物理按键、触摸）
聚合为统一事件流，LVGL 线程经 `espaperplay_input_try_get_key()` /
`espaperplay_input_try_get_touch()` 分别直读两类队列。

**物理按键** —— BOOT（GPIO0）、音量加（GPIO2）、音量减（GPIO5）均低电平
有效，由官方
[`espressif/button`](https://components.espressif.com/components/espressif/button)
组件（v4.2.0，组件管理器依赖）驱动，原始事件归一化为
`espaperplay_input_key_action_t`：PRESS_DOWN / PRESS_UP / SINGLE_CLICK /
DOUBLE_CLICK / LONG_PRESS_START / LONG_PRESS_HOLD / LONG_PRESS_UP（含按压
时长）。`LONG_PRESS_HOLD` 按每个按键独立节流为每 500ms 一个——驱动默认每 20ms 触发一次，
会洪泛事件管道并导致 LONG_PRESS_UP 漏检。

**双队列（按键 / 触摸隔离）** —— 两个独立 FreeRTOS 队列：

| 队列 | 深度 | 满时策略 |
| ---- | ---- | -------- |
| 按键 | 16 | 队首投递 + 挤掉最旧——按键事件永不丢失 |
| 触摸 | 32 | `xQueueSend` 满时丢新——保留中间点供轨迹绘制 |

触摸采用中断驱动（GT911 INT 唤醒任务——I2C 不能在 ISR 中执行——任务读取
坐标后投递触摸队列），高频触摸流量不会挤占按键队列。LVGL 线程每个 indev
read 周期（~30ms）全量排空触摸队列，积压有界。

**GUI 输入消费** —— 两个队列都在 LVGL 线程内直读
（`espaperplay_ui_input_start()`），无独立分发任务、无跨线程投递：

- **音量键**：单击/长按保持按 10% 步进调整全局音量，持久化 NVS 并更新音频
  软件增益；音量键不进入页面导航。
- **按键**：按键泵 lv_timer（20ms 周期）排空按键队列，逐条转发给栈顶页面
  的可选 `on_key` 钩子（`espaperplay_ui_page_t` 扩展）。导航决策属于页面；
- **长按默认动作**：BOOT 键的 `LONG_PRESS_START` 在页面栈按键汇流处全局
  拦截（先于页面钩子），**每次长按只响应一次**（HOLD / 松开事件不再触发）：
  `full_refresh`（默认——强制整屏深刷新清残影）、`back`（返回上一页）或
  `none`。对所有页面统一生效，可在设备端设置页或 Web 管理页配置
  （`boot_long_press_action`，NVS 持久化，改完立即生效）；长按判定时间
  同样可配置（`boot_long_press_time_ms`，默认 1000ms，范围 300-10000ms，
  启动时应用到按键驱动，修改即时生效）；
- **触摸**：触摸指针 indev 的 `read_cb` 排空触摸队列（见 `lvgl_touch.c`）：
  队列中的每个事件逐条转发给栈顶页面的可选 `on_touch` 钩子（全部中间
  坐标保留，供轨迹绘制），按压状态机驱动 LVGL 控件。点击锁存 FIFO 会把
  LVGL 线程忙碌期间完整到达的按下+释放补报回放，快速连击不被吞。

- 主界面应用卡片启动各应用（天气 / 阅读器 / 文件 / 设置）；
- 测试页（局刷压力测试 + 按键/触摸实时显示，入口在**设置页 → 系统 →
  测试页**）：触摸画板把手指轨迹画成连续折线（8 笔 × 512 点，`clear`
  按钮清空画板，`back` 按钮经 LVGL 指针 indev 点击返回）；长按松开 →
  返回。

上电自检（`ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST`，默认关闭）通过真实输入队列
注入合成按键事件，断言页面栈响应（1→2→1→2），输出
`key selftest PASS/FAIL`。

### 设备端设置页

`screen_settings`（主界面 → 设置）是设备上的控制面板：

- 分类首页包含网络与连接、显示、按键、阅读器、天气服务、系统。点击分类
  打开二级详情；点击左上返回、边缘返回手势或 BOOT 单击先返回分类，再返回首页。
- 整行点击；数值项（显示驱动休眠、连续局刷强制全刷阈值、BOOT 长按判定
  时间）弹出 `[-] [+]` 步进模态；点击保存后持久化 NVS 并同步应用到
  运行中的服务，关闭弹窗不保存。
- BOOT 键长按动作（全刷 / 返回 / 无操作）与 WiFi 模式（热点 / 站点，带
  二级确认）点击循环切换。
- **WiFi 网络**打开扫描列表页（`screen_wifi_list`）：信号强度 + 加密
  标记、当前网络高亮、加密网络弹密码键盘、连接中视图可取消；取消 / 超时
  自动恢复软 AP，保证 Web 管理页始终可达。
- 墨水屏上难以输入的配置项（WiFi 凭据、和风 API Key / 位置 / API Host）
  只展示状态并提示去 Web 管理页配置。
- 字体选择（出厂内置 + SD 卡字体）、固件版本行、系统分类（测试页入口）。

### 统一视觉规范（2026-09）

- 首页保留 64px 图标与一行四个入口；移除品牌字样，日期和星期合并为一行。
- 共用 30px 状态栏、16px 页面边距、1px 黑色边框、10px 卡片圆角；白底黑字，
  不依赖灰色、阴影和动画区分状态。阅读正文沿用独立的可调字号。
- 设置与天气采用应用标题区、清晰的文字层级和明确的返回入口。设置名称和
  当前值上下排列，长值单行省略；各分类保留原有配置操作与确认行为。
- 显示分类提供「屏保预览」，不改变自动睡眠设置，触摸即可返回。
- 天气提供「现在 / 预报 / 生活」标签。逐小时区域点击或横向滑动切换前后
  12 小时；逐日预报以列表显示高低温；日月天象采用独立图标和时间，月相
  单独标注文字；生活指数支持分页和点击查看完整建议，预警支持逐条查看。
- 文件页采用路径、项目数量和带文件类型/大小的分隔列表，底部保留上一级与
  新建文件夹；页码和前后页入口支持点击及横向滑动。每次切页同步重建命中区域。
- 书架采用顶部「最近阅读 / 全部图书」标签、横向封面与两行书名、阅读进度；
  底部显示页码，封面后台加载不影响分页。
- 阅读工具面板分为标题、翻页/页码跳转、目录/字号/灰度/书架三层；目录使用
  两行章节标题和当前章节标记，跳转页码复用数字键盘弹窗。
- 显示驱动休眠等数值项使用紧凑步进弹窗，区分说明、当前值、取消与保存。
- 引导页根据当前联网模式显示连接说明，本机配置列表提供明确的返回入口。
