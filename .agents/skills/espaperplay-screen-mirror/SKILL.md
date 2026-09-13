---
name: espaperplay-screen-mirror
description: ESPaperPlay 墨水屏远程镜像与远程操作调试——通过设备 Web 控制台拉取墨水屏当前画面并渲染成 PNG 供 agent 直接查看，或经 WebSocket 注入触摸点击 / BOOT 键动作 / 强制全刷来远程操作设备。凡是在 ESPaperPlay 项目中需要"看设备屏幕、截图墨水屏、取设备当前画面、上板看 UI 效果、远程点一下设备、模拟触摸验证界面、等待刷屏后确认画面"时使用，即使用户没有说出"镜像"两个字。
---

# ESPaperPlay 屏幕镜像调试

设备固件内置屏幕镜像服务：Web 控制台（HTTPS :443，自签证书）提供
`GET /api/screen/snapshot`（当前画面完整帧）与 `GET /api/screen/ws`
（实时推流 + 远程注入）。配套脚本 `scripts/mirror.py`（纯 stdlib）封装了
登录、快照解码存 PNG、触摸/按键注入。把它当作你在这块设备上的"眼睛和手指"：
调 UI、验证刷屏效果、复现触摸问题时，先看画面再操作，不要只靠用户口述。

## 快速开始

1. 确定设备地址（STA IP 或 AP 模式的 `192.168.4.1`）：问用户，或从串口日志
   的 `sta ip` / `ESPaperPlay_WEB` 启动行附近获取。
2. 取会话：`--password "<控制台密码>"` 首次登录（token 自动缓存到 /tmp），
   之后同 host 自动复用；或 `--token` 直接指定。
3. 看画面并存 PNG：

   ```bash
   python3 .agents/skills/espaperplay-screen-mirror/scripts/mirror.py \
       --host <IP> --password <密码> snap -o /tmp/esp_screen.png
   ```

   然后**用 Read 工具查看该 PNG**——这就是设备屏幕当前显示的内容。
4. 远程点击并确认效果（e-ink 刷新有延迟，操作后等 1~3s 再 snap）：

   ```bash
   mirror.py --host <IP> tap 400 240     # 点击屏幕中心
   sleep 3 && mirror.py --host <IP> snap -o /tmp/after.png
   ```

## 能力与命令

| 命令 | 作用 |
| ---- | ---- |
| `login` | 登录并缓存 token（验证凭据 / 刷新过期会话） |
| `snap -o x.png` | 拉当前画面存 PNG（黑白 1bpp / 四灰 2bpp 自动识别） |
| `watch -n 5 -i 1.0 -d dir/` | 轮询快照序列；seq 未变自动跳过，适合等待刷屏 |
| `tap X Y` | 注入一次点击（down +150ms +up） |
| `touch X Y down\|up` | 注入单条触摸帧（拖动 = 多条 down + 最后 up） |
| `key click\|double_click\|long_press_start\|press_down\|press_up` | 模拟 BOOT 键 |
| `refresh` | 请求强制全刷（清残影，约 1.7~2.5s） |
| `live -n 3 -d dir/` | WS 连接连收 N 帧（设备每次刷屏推一帧） |

坐标即面板物理坐标，与快照 PNG 中的位置一一对应（0..799 × 0..479）。
注意：UI 页面可能以竖屏排版旋转绘制在 800×480 原始帧里（如主屏时钟），
但注入坐标**不需要**自行旋转——LVGL 内核按显示旋转自动换算，点你在
PNG 里看到的位置即可（真机已验证：点主页"设置"图标打开设置页）。

## 排障

- **HTTP 401**：token 过期/失效 → 带 `--password` 重新 login。
- **无法连接 / 超时**：设备很可能在自动浅睡眠（无操作 70s 后）→ 重试几次，
  或请用户按一下屏幕唤醒；连接成功后控制台心跳会抑制睡眠，但脚本单项命令
  很短，建议连续调试时用 `watch`/`live` 保持会话活跃。
- **HTTP 503 镜像画布未就绪**：开机后尚无任何刷屏（罕见），先 `refresh` 再 snap。
- **WS 握手被关闭**：token 无效（重新 login）或固件过旧（无 WS 路由）。
- 403/证书报错：脚本默认跳过自签证书校验，无需额外处理。

## 协议速查（读懂设备端日志 / 扩展脚本用）

- 二进制帧 = 16 字节头 + 位图：`'EPM1' | ver=1 | fmt(0=1bpp,1=2bpp) |
  seq u32le | w u16le | h u16le | 保留2B`。1bpp 位 1=白；2bpp 0=白 1=浅灰
  2=深灰 3=黑，均 MSB 在前、按行压缩。
- WS 文本消息（客户端→设备）：`{"t":"touch",x,y,down}`、
  `{"t":"key",action}`、`{"t":"refresh"}`、`{"t":"ping"}`；设备只对
  错误回 `{"t":"err",msg}`，成功静默。
- 设备端实现：`components/services/webserver/src/webserver_handler_screen.c`
  （路由/鉴权/注入）与 `webserver_screen_mirror.c`（画布/推送）；
  注意 WS URI 条目必须显式 `is_websocket = true`（IDF v6.1 握手不进 handler）。
- 远程注入与 GT911 真实触摸走同一条事件路径；控制台密码未配置的首次
  设置阶段，镜像接口与控制台其他敏感接口一致地放行。
