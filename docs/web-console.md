# Web Console · Web 管理控制台

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

<a id="en"></a>
## English

### Web Console

After boot, a management page is served over **HTTPS** on port **443** of every
network interface (reachable in both AP and STA modes). Plain **HTTP on port
80** is still listened to, but only redirects every request to HTTPS (no
plaintext traffic). The device generates its own P-256 self-signed certificate
on first boot (persisted in NVS, fingerprint stays stable across reboots); the
private key never leaves the device, so it cannot be extracted from the firmware.

| Route | Method | Description |
| ----- | ------ | ----------- |
| `/` | GET | Management page (embedded HTML, sidebar multi-level menu) |
| `/api/status` | GET | Runtime status: uptime / heap / firmware / WiFi (auth) |
| `/api/config` | GET | Current system config: WiFi mode & credentials (auth) |
| `/api/config` | POST | Update config (form-encoded), save & re-apply WiFi (auth) |
| `/api/config/reset` | POST | Restore factory defaults & re-apply WiFi (auth) |
| `/api/wifi/restart` | POST | Re-apply WiFi config (auth) |
| `/api/wifi/scan` | GET | Scan nearby networks (SSID / RSSI / auth, deduped) (auth) |
| `/api/setup/status` | GET | First-boot wizard state (pre-password setup phase) |
| `/api/setup/apply` | POST | Apply first-boot setup (network / weather) |
| `/api/system/reboot` | POST | Reboot the device (auth) |
| `/api/system/touch_diag` | POST | Trigger GT911 touch diagnostics / recovery (auth) |
| `/api/heartbeat` | GET/POST | Web console liveness — suppresses auto light sleep |
| `/api/files` | GET | List an SD card directory (auth) |
| `/api/files/upload` | POST | Upload a file to the SD card (auth) |
| `/api/files/download` | GET | Download a file from the SD card (auth) |
| `/api/files/mkdir` | POST | Create a directory (auth) |
| `/api/files/rename` | POST | Rename / move a file (auth) |
| `/api/files/delete` | POST | Delete a file / directory (auth) |
| `/api/fonts` | GET | List SD fonts + active font selection (auth) |
| `/api/fonts/upload` | POST | Upload a font file (.ttf/.otf/.ttc) (auth) |
| `/api/fonts/select` | POST | Select the active font (auth) |
| `/api/fonts/delete` | POST | Delete a font file (auth) |
| `/api/auth/status` | GET | Login state: authenticated / password configured |
| `/api/auth/login` | POST | Password login, returns a session token |
| `/api/auth/password` | POST | Set (first-time) or change password (auth if set) |
| `/api/auth/logout` | POST | Revoke the current session |
| `/api/weather` | GET | Weather service status + data snapshot summary (auth) |
| `/api/weather/refresh` | POST | Request an immediate weather refresh (auth) |

> AP mode: connect to the device AP from a phone / laptop and browse to
> `http://192.168.4.1/` (it redirects to `https://192.168.4.1/`). Your browser
> will warn about the self-signed certificate — accept the security exception
> once; the certificate SHA-256 fingerprint is printed in the serial log so you
> can verify the connection. On first boot (no password configured yet) the
> console guides you to set a password; afterwards login is required.
> Sensitive APIs (config / wifi / files / fonts / reboot / status) are protected
> by a bearer session token; 5 consecutive login failures trigger a temporary
> lockout.

---

<a id="zh"></a>
## 简体中文

### Web 管理控制台

设备启动后会在**所有网络接口的 443 端口**以 **HTTPS** 提供管理页面（AP 与
STA 模式下均可访问）。**80 端口**仍会监听，但仅把所有请求 302 重定向到
HTTPS，杜绝明文流量。设备在首次启动时自行生成 P-256 自签名证书（持久化于
NVS，重启后指纹不变），私钥永不出设备，无法从固件中提取。

| 路由 | 方法 | 说明 |
| ---- | ---- | ---- |
| `/` | GET | 管理页面（嵌入式 HTML，侧边栏多级菜单） |
| `/api/status` | GET | 运行状态：运行时间 / 堆 / 固件 / WiFi 等（需登录） |
| `/api/config` | GET | 当前系统配置：WiFi 模式与凭据（需登录） |
| `/api/config` | POST | 更新配置（表单编码），保存并重新应用 WiFi（需登录） |
| `/api/config/reset` | POST | 恢复出厂默认配置并重新应用 WiFi（需登录） |
| `/api/wifi/restart` | POST | 重新应用 WiFi 配置（需登录） |
| `/api/wifi/scan` | GET | 扫描附近网络（SSID / 信号 / 加密，去重）（需登录） |
| `/api/setup/status` | GET | 首次开机向导状态（未设密码的引导阶段） |
| `/api/setup/apply` | POST | 应用首次开机引导配置（网络 / 天气） |
| `/api/system/reboot` | POST | 重启设备（需登录） |
| `/api/system/touch_diag` | POST | 触发 GT911 触摸诊断 / 自愈（需登录） |
| `/api/heartbeat` | GET/POST | Web 管理页心跳——抑制自动浅睡眠 |
| `/api/files` | GET | 列出 SD 卡目录（需登录） |
| `/api/files/upload` | POST | 上传文件到 SD 卡（需登录） |
| `/api/files/download` | GET | 从 SD 卡下载文件（需登录） |
| `/api/files/mkdir` | POST | 新建目录（需登录） |
| `/api/files/rename` | POST | 重命名 / 移动文件（需登录） |
| `/api/files/delete` | POST | 删除文件 / 目录（需登录） |
| `/api/fonts` | GET | 列出 SD 卡字体与当前选用（需登录） |
| `/api/fonts/upload` | POST | 上传字体文件（.ttf/.otf/.ttc）（需登录） |
| `/api/fonts/select` | POST | 选用当前字体（需登录） |
| `/api/fonts/delete` | POST | 删除字体文件（需登录） |
| `/api/auth/status` | GET | 登录状态：是否已登录 / 是否已设置密码 |
| `/api/auth/login` | POST | 密码登录，成功后返回会话 token |
| `/api/auth/password` | POST | 首次设置 / 修改密码（已设置后需登录） |
| `/api/auth/logout` | POST | 吊销当前会话 |
| `/api/weather` | GET | 天气服务状态与数据快照摘要（需登录） |
| `/api/weather/refresh` | POST | 请求立即刷新天气数据（需登录） |

> AP 模式下用手机 / 电脑连接设备热点，浏览器访问 `http://192.168.4.1/`
>（会自动重定向到 `https://192.168.4.1/`）。浏览器会对自签名证书给出安全
> 警告，首次访问需手动信任一次；证书 SHA-256 指纹会打印在串口日志中，可据
> 此核对连接真实性。设备出厂未设置密码时，页面会引导首次设置密码；
> 设置后访问需登录。
> 配置 / WiFi / 文件 / 字体 / 重启 / 状态等敏感接口由会话 token 鉴权保护，
> 连续 5 次登录失败会触发临时锁定。
