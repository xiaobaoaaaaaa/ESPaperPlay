# Storage & File Manager · 存储与文件管理

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

<a id="en"></a>
## English

### MicroSD Storage: SDIO (SDMMC) + FATFS

The storage stack follows the project's two-layer split: a low-level MicroSD
driver (`components/drivers/sd`) drives the card over the on-chip **SDMMC
host (SDIO protocol stack)**, and the storage service
(`components/services/storage`) layers FAT + VFS on top, exposing everything
under the mount point `/sdcard` through standard C file APIs (`fopen` /
`fread` / `fwrite` …).

**Driver layer (`espaperplay_sd_*`)** — runs the SDIO bring-up sequence:
`sdmmc_host_init()` → `sdmmc_host_init_slot()` →
`sdmmc_card_init()` (CMD0/CMD8/ACMD41/CMD2/CMD3/CMD9/CMD7), prints card info
and exposes raw sector I/O (`espaperplay_sd_read/write_sectors()`). All SDMMC
signals are routed through the GPIO matrix, so any GPIO works; the pins chosen
here (see `espaperplay_config.h` "SD" section) deliberately avoid the EPD SPI
(7–13), touch (2–5) and UART0 (43/44) pins, unlike the IDF defaults
(D0=2/D1=4/D2=12/D3=13 would collide). **GPIO19/20 are the chip's USB D-/D+
pads** (board USB / secondary USB-Serial-JTAG console) and must never be used
for SD — an earlier draft mapped D3=19 and a card-power pin on 20, and the
device lost USB and reset-looped immediately. 4-bit mode needs external pull-ups on CMD+D0-D3; the
driver also enables the internal pull-ups (`SDMMC_SLOT_FLAG_INTERNAL_PULLUP`).
SDIO needs no chip-select; GPIO21 (the old SPI-mode CS guess) is reused for
D3 since it was already expected to reach the SD socket.

| Signal | GPIO |
| ------ | ---- |
| CLK    | 14   |
| CMD    | 15   |
| D0     | 16   |
| D1     | 17   |
| D2     | 18   |
| D3     | 21   |

Clock is 20 MHz by default (SDMMC_FREQ_DEFAULT); 4-bit SDIO at 20 MHz already
outperforms the old SPI plan (≈10 MB/s vs ≈2.5 MB/s peak), and the host
supports up to 40 MHz SDR if the card and layout allow.

**Storage service (`espaperplay_storage_*`)** — `espaperplay_storage_mount()`
calls the SD driver, allocates a free FAT drive number
(`ff_diskio_get_drive`), registers the card as a FATFS disk
(`ff_diskio_register_sdmmc`), registers the VFS prefixed path
(`esp_vfs_fat_register`) and runs `f_mount`. It deliberately does **not**
auto-format: a card without a FAT32 filesystem fails cleanly instead of
silently wiping data. `espaperplay_storage_unmount()` reverses the whole chain
in order (unmount → unregister VFS → unregister disk → SD deinit).

A boot without a card is tolerated: `main.c` logs a warning and the device
keeps working (net clock / weather / web console / UI), only file-based
features (reader / file manager) are unavailable. FATFS is configured for LFN
on the heap and UTF-8 API encoding (`sdkconfig.defaults`), so Chinese
filenames and book content on the card are read/written as UTF-8.

Optional acceptance self-tests (both off by default): the driver self-test
(`ESPAPERPLAY_SD_ENABLE_SELFTEST`) reads sector 0 and the last sector (read
only, non-destructive), and the storage self-test
(`ESPAPERPLAY_STORAGE_ENABLE_SELFTEST`) writes/reads/deletes a temporary file
after mount, proving the SDIO → FATFS → VFS chain end to end.

### File Manager

- **On device** (`screen_files`, home → 文件): browse the SD card; enter
  directories; TXT / EPUB open directly in the reader; other files show an
  info modal. Long-press opens a per-file action menu. Missing / removed
  cards are handled gracefully (no crash, friendly hints).
- **On the Web console** (`/api/files*`): full management — list / upload /
  download / mkdir / rename / delete — so books and fonts can be loaded from
  a phone or PC without pulling the card.

---

<a id="zh"></a>
## 简体中文

### MicroSD 存储：SDIO（SDMMC）接口 + FATFS

存储栈沿用项目的双层划分：底层 MicroSD 驱动（`components/drivers/sd`）基于
片上 **SDMMC 主机（SDIO 协议栈）** 驱动卡片，存储服务
（`components/services/storage`）在其上叠加 FAT + VFS，把内容统一挂到
`/sdcard` 挂载点，通过标准 C 文件 API（`fopen` / `fread` / `fwrite` ...）
直接访问。

**驱动层（`espaperplay_sd_*`）** —— 执行 SDIO 上电时序：`sdmmc_host_init()` →
`sdmmc_host_init_slot()` → `sdmmc_card_init()`（CMD0/CMD8/ACMD41/CMD2/CMD3/
CMD9/CMD7），并打印卡片信息，对外暴露底层扇区读写
（`espaperplay_sd_read/write_sectors()`）。SDMMC 信号经 GPIO 矩阵路由、可
任意选脚；本板选用的引脚（见 `espaperplay_config.h` 的 SD 节）刻意避开 EPD
SPI（7-13）、触摸（2-5）与 UART0（43/44），而 IDF 默认引脚（D0=2/D1=4/
D2=12/D3=13）会与它们冲突。**GPIO19/20 是芯片内置 USB D-/D+ 焊盘**（板载
USB / 次级 USB-Serial-JTAG 串口），严禁用于 SD——先前版本曾把 D3=19 与卡片
电源脚放在 20，设备立即失去 USB 并复位循环。4-bit 模式要求 CMD 与 D0-D3 外接上拉，
驱动同时开启内部上拉（`SDMMC_SLOT_FLAG_INTERNAL_PULLUP`）作为补充。SDIO
模式不需要片选——GPIO21（原 SPI 方案的片选猜测脚）被复用作 D3，因为它
最可能已连到 SD 卡座。

| 信号 | GPIO |
| ---- | ---- |
| CLK  | 14   |
| CMD  | 15   |
| D0   | 16   |
| D1   | 17   |
| D2   | 18   |
| D3   | 21   |

时钟默认 20MHz（SDMMC_FREQ_DEFAULT）。4-bit SDIO 在 20MHz 下吞吐已远超原
SPI 方案（峰值约 10MB/s 对 2.5MB/s），主机最高支持 40MHz SDR（视卡片与
布线而定）。

**存储服务（`espaperplay_storage_*`）** —— `espaperplay_storage_mount()` 先调用
SD 驱动，再分配空闲 FAT 卷号（`ff_diskio_get_drive`）、把卡片注册为 FATFS
底层磁盘（`ff_diskio_register_sdmmc`）、注册 VFS 前缀路径
（`esp_vfs_fat_register`）并执行 `f_mount`。刻意**不自动格式化**：卡片没有
FAT32 文件系统时干净地报错返回，绝不静默擦除已有数据。
`espaperplay_storage_unmount()` 逆序释放整条链路（卸载 → 注销 VFS → 注销
磁盘 → SD 驱动反初始化）。

无卡启动被容忍：`main.c` 记录警告后继续运行（网络时钟 / 天气 / Web 管理 /
UI 不受影响），仅文件类功能（阅读器 / 文件管理）不可用。FATFS 已配置为
堆上 LFN + UTF-8 API 编码（`sdkconfig.defaults`），卡片上的中文文件名与
书籍内容按 UTF-8 读写。

可选的验收自检（均默认关闭）：驱动级自检（`ESPAPERPLAY_SD_ENABLE_SELFTEST`）
只读校验扇区 0 与最后一扇区（非破坏、不写数据），存储级自检
（`ESPAPERPLAY_STORAGE_ENABLE_SELFTEST`）在挂载后写入/读回/删除临时文件，
端到端验证 SDIO → FATFS → VFS 链路。

### 文件管理

- **设备端**（`screen_files`，主界面 → 文件）：浏览 SD 卡、进入目录；
  TXT / EPUB 直接打开阅读器；其他文件弹出信息模态。长按弹出单文件操作
  菜单。缺卡 / 拔卡优雅处理（不崩溃，友好提示）。
- **Web 管理页**（`/api/files*`）：完整管理——列表 / 上传 / 下载 / 新建
  目录 / 重命名 / 删除——手机电脑即可装书装字体，无需拔卡。
