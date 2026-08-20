# Changelog

本文件记录 ESPaperPlay 的所有显著变更。

格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。版本号唯一来源是
`components/board/include/espaperplay_config.h` 中的 `ESPAPERPLAY_VERSION_*`。

## [未发布]

## [0.1.0] - 2026-08-20

首个公开版本：ESP32-S3 电子纸设备平台（7.5" 800×480 EPD，16MB flash / 8MB PSRAM）。

### 新增

- **电子纸（EPD）驱动**（GDEY075T7-T01 / UC8179）
  - 全屏 / 局部差分（N2OCP）/ 快刷 / 4 灰阶刷新模式
  - 强制全像素翻转全刷（FULL_FORCE）清除残影；初始化后清白全刷建立基线
  - 空闲自动深度睡眠（默认 90s，可配置），唤醒自动重新初始化
  - 深度睡眠唤醒首刷全屏基线，消除偶发残影 / 刷新不完整
- **GUI 渲染后端**（RGB565 主帧 + 模式化转换）
  - BW：阈值 / Bayer 抖动；GRAY4：Floyd–Steinberg / Bayer
  - 异步刷新 worker：渲染线程零阻塞、双槽位排队与脏区合并
  - 连续局刷计数强制全刷清残影（无论区域大小，阈值可配置，默认 10）
- **LVGL 集成**
  - FreeType 矢量字体渲染（fonts 分区 + Noto Sans SC 中文子集）
  - 主界面安卓风格桌面（状态栏 + 时钟/应用双页 + 滑动切换 + Iconify 图标）
  - 页面栈导航、百分比布局（适配不同分辨率/横竖屏）、双击旋转屏幕
- **输入**
  - BOOT 物理按键（espressif/button）：短按 / 长按全局默认动作可配置（全刷 / 返回 / 无操作）
  - GT911 电容触摸（I2C，复位地址锁存、INT 中断读取）
- **MicroSD 卡存储**（SDIO/SDMMC 接口）
  - SDMMC 主机 / 槽位 / 卡片底层驱动：CLK=14、CMD=15、D0–D3=16-18/21，PWR 默认关闭
  - 引脚避让 GPIO19/20（芯片内置 USB D-/D+ 焊盘，避免与板载 USB/串口冲突导致断连重启）
  - 卡片探测 / 信息 / 底层扇区读写 API，含可选只读自检
  - FATFS 挂载 + VFS 注册（LFN 长文件名、UTF-8 编码，中文文件名与内容不乱码）
  - SD 卡缺失或未格式化不阻断启动：网络 / 天气 / Web / UI 照常，仅文件类功能（阅读器）不可用
- **网络与时间**
  - WiFi AP / STA 模式（WPA2/WPA3）
  - 和风天气（QWeather）全部 API + 天气页（实时条件 / 24h 气温曲线 / 图标集）
  - netip → geoip → 时区 → NTP 自动对时（带 TTL 内存缓存）
- **Web 管理**
  - HTTPS（运行时生成自签名证书，私钥不出设备）
  - 鉴权（PBKDF2 加盐）与会话管理，登录失败限速锁定
  - 首次使用免密码引导设置管理密码
  - 可配置：WiFi、屏幕空闲超时、全刷阈值、BOOT 长按动作、天气
- **板级 / 构建**
  - 自定义分区表（APP 4MB、fonts 8MB SPIFFS 只读资产）
  - -O2 编译优化 + CPU 动态调频（DFS 240/80MHz）
  - 版本号单一来源（config.h 数字宏），CMake `PROJECT_VER` 同步

### 修复

- EPD 局部刷新条纹伪影（N2OCP 差分架构替代影子缓冲）
- 灰阶切回黑白时的中间灰残留
- 深度睡眠唤醒后首次局刷残影 / 刷新不完整
- 局刷图像错位（GUI 槽位竞态 + 驱动快照）
- GT911 复位时序，开机数分钟无响应
- 天气服务偶发连接失败（LWIP 连接池耗尽）、响应缓冲内存碎片化
- Web 保存 WiFi 配置时先响应再重启
- 空闲自动睡眠定时器重新武装失效
- 屏幕旋转后控件命中错位

### 性能

- 快刷改用 OTP 波形 + PLL 100Hz（实测约 2.4s → 1.1s）
- 局刷 PTOUT 时序调整（实测提速 0~13%）
- TLS 收发缓冲回落 PSRAM + 动态缓冲，防止内部堆碎片化
- LWIP 连接资源池容量调优

### 已知限制

- 暂无 OTA（串口烧录，分区为 factory 无 A/B）
- 首次使用需设置 Web 管理密码（出厂免密码仅限首次设置阶段）
- E-ink 刷新固有闪烁；全刷阈值 / 睡眠时长可在 Web 管理页按需调整

[0.1.0]: https://github.com/xiaobaoaaaaaa/ESPaperPlay/releases/tag/v0.1.0
