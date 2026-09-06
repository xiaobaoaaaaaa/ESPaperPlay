# Power & Reliability · 电源与可靠性

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

<a id="en"></a>
## English

### Power Management

The power service (`components/services/power`) implements the low-power
cycle around **auto light sleep**:

- **Activity tracking** — touch, keys and refreshes note user activity; after
  an idle timeout (configurable on the device settings page and Web console,
  NVS-persisted) the device enters light sleep automatically. The Web console
  sends heartbeats (`/api/heartbeat`) while open, suppressing auto sleep so
  remote sessions are not interrupted.
- **Before sleeping** — WiFi is disconnected deliberately and auto-reconnect
  suppressed; wakeup sources are configured (touch, BOOT, timer).
- **Minute-aligned timer wakeup** — while asleep the device wakes on whole
  minutes to refresh weather and the clock, so the screen is up to date the
  moment you wake it. Wake/sleep races are handled (touch during the wake
  window cancels re-sleep; phantom frames after resume are dropped; the
  status-bar sleep icon clears).
- **Software RTC drift calibration** — the periodic timer wakeups double as
  drift measurements for the internal RC oscillator (sleep time only), so
  long sleep periods keep wall-clock accuracy without an external 32k crystal.
- The EPD panel additionally deep-sleeps itself after 90 s without refresh
  (see the [EPD driver](display.md)).

### Touch Reliability (GT911 Self-Healing)

The GT911 on this board has a known failure mode: if INT is sampled high
during reset, the controller latches I2C address `0x14` instead of `0x5D`, and
EPD refreshes on the shared rail can induce internal power-on resets that
trigger it in the field. The touch driver therefore:

- polls controller health periodically (~5 s cadence);
- on failure, runs a diagnostic + recovery sequence (reset with INT guarded,
  re-detect the latched address, re-init) with exponential backoff, so a dead
  touch panel self-heals without a power cycle;
- exposes a remote diagnostic trigger (`POST /api/system/touch_diag`) from
  the Web console for bench debugging.

### Diagnostics Logging

The `diaglog` service appends diagnostic records to a file on the SD card
(stack-safe write API usable from small-stack tasks): boot / sleep / wake /
touch-recovery lifecycle events build a failure timeline across reboots, and a
periodic heartbeat log carries internal-RAM and DMA-capable heap statistics,
so field failures can be reconstructed from the card afterwards.

---

<a id="zh"></a>
## 简体中文

### 电源管理

电源服务（`components/services/power`）围绕**自动浅睡眠**实现低功耗循环：

- **活动追踪**——触摸、按键、刷新都会记录用户活动；空闲超时（设备端
  设置页与 Web 管理页均可配置，NVS 持久化）后自动进入浅睡眠。Web 管理
  页打开期间发送心跳（`/api/heartbeat`）抑制自动睡眠，远程操作不被打断。
- **睡前**——主动断开 WiFi 并抑制自动重连；配置唤醒源（触摸 / BOOT /
  定时器）。
- **分钟对齐定时唤醒**——睡眠中整分唤醒，刷新天气与时钟，亮屏瞬间数据
  即新。唤醒 / 睡眠竞态已处理（唤醒窗口内的触摸不再立刻又睡；恢复后的
  幻影帧被丢弃；状态栏睡眠图标正常清除）。
- **软件 RTC 漂移标定**——周期定时唤醒兼作内部 RC 振荡器（仅睡眠时段）
  的漂移测量，无外部 32k 晶振也能长期保持走时精度。
- EPD 面板自身另有 90s 无刷新自动深睡（见 [EPD 驱动](display.md)）。

### 触摸可靠性（GT911 自愈）

本板 GT911 有一个已知失效模式：复位期间 INT 被采样为高电平时，控制器把
I2C 地址锁存为 `0x14` 而非 `0x5D`；共轨的 EPD 刷新可能诱发内部上电复位，
现场使用中就会触发。触摸驱动因此：

- 周期巡检控制器健康（约 5s 节奏）；
- 失效时执行诊断 + 恢复序列（INT 守卫下复位、重检测被翻转的地址、重新
  初始化），带指数退避重试——触摸失效无需断电重启，自动恢复；
- 提供 Web 管理页远程诊断触发（`POST /api/system/touch_diag`），方便
  台架调试。

### 诊断日志

`diaglog` 服务把诊断记录追加到 SD 卡文件（写入 API 小栈安全，小栈任务
可放心调用）：开机 / 睡眠 / 唤醒 / 触摸自愈等生命周期事件跨重启构建
失效时间线；周期心跳日志附带内部 RAM 与 DMA 可用堆统计——现场故障事后
可凭卡内日志复盘。
