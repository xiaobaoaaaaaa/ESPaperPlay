# Fonts · 字体系统

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

<a id="en"></a>
## English

### Read-Only Font Assets (fonts Partition)

The fonts service (`components/graphics/fonts`) ships TrueType fonts as
read-only flash assets:

- `assets/fonts/` holds the font files. `NotoSansSC_Regular.ttf` (2.2 MB) is a
  subset of Noto Sans SC covering GB2312 level-1+2 (6763 common CJK glyphs),
  ASCII and common CJK punctuation; regenerate with
  `tools/fonttools-venv/bin/python tools/prepare_font.py` (downloads the
  variable font once into `tools/font_src/`, gitignored).
- At build time `spiffs_create_partition_assets` (from `esp_mmap_assets`)
  packs the directory into a SPIFFS image for the `fonts` partition (8 MB,
  subtype `spiffs`, see `partitions.csv`) and generates
  `mmap_generate_fonts.h` (file index + checksum) into the component build
  dir. `idf.py flash` flashes the partition image automatically.
- At runtime `espaperplay_fonts_init()` (called after LVGL starts) maps the
  partition with `esp_mmap_assets` — font bytes stay in flash, **zero RAM
  copies** — and registers it as LVGL filesystem drive `A:` via `esp_lv_fs`.
  Fonts are then opened as LVGL paths, e.g. `A:NotoSansSC_Regular.ttf`
  (`espaperplay_fonts_get_path()` builds the path).

Note: only file names with `[A-Za-z0-9_]` are allowed — hyphens break the
generated C enum (`MMAP_FONTS_*`); also keep names **shorter than
`CONFIG_MMAP_FILE_NAME_LENGTH` (default 16, set to 32 here)** — an over-long
name is silently truncated at build time (Warn only), and the runtime
`strcmp` lookup then fails so the font cannot be opened (symptom:
`lv_freetype_font_create` returns NULL with no useful FT error).

#### Full Font from SD Card (Priority)

Besides the trimmed flash subset, the device can serve a **full** font from the
SD card when one is inserted and mounted (the flash `fonts` partition is only an
8 MB trimmed subset; the full Noto Sans SC is ~10 MB):

- When the SD card is mounted, the fonts component registers an **additional
  LVGL filesystem drive** `B:` (`espaperplay_sd_fonts.c`) that proxies
  `fopen`-style access to the SD directory `ESPaperPlay_FONTS_SD_DIR` (default
  `/sdcard/system/fonts/`, under the `system` top-level dir so firmware data stays
  separate from user content like novels/EPUB in the SD root). Registering drive
  `B:` is a pure callback registration and is safe even when no card is present
  at boot.
- `espaperplay_fonts_load(name, size, style)` resolves the source per load:
  1. If the SD card is mounted **and** `/sdcard/system/fonts/{name}` exists →
     load it (full font, drive `B:`), logging `(SD full)`;
  2. Otherwise fall back to the flash subset (drive `A:`), logging
     `(Flash subset)`.
- Putting `NotoSansSC_Regular.ttf` in `/sdcard/system/fonts/` therefore makes the
  whole device render with the full CJK coverage instead of the GB2312 subset.
- **Managing fonts from the Web UI:** the Web console exposes a *字体管理*
  (Font Management) section where you can **upload** font files (`.ttf` / `.otf` /
  `.ttc`) to the SD card, **list** what is already there, and **select** the active
  font (persisted to NVS). The selected font is used when present on the SD card;
  otherwise the device falls back to the built-in flash subset so rendering never
  breaks. A link to Google Fonts (Noto Sans SC) is shown as a download hint. The
  new selection takes effect after a **reboot**.

### FreeType Font Rendering

Rendering is built on the **LVGL 9.5 FreeType wrapper** (`src/libs/freetype/`,
wrapper only) plus the external
[`espressif/freetype`](https://components.espressif.com/components/espressif/freetype)
component (the real libfreetype 2.14.3, registry dependency):

- Kconfig (`sdkconfig.defaults`): `CONFIG_LV_USE_FREETYPE=y`,
  `CONFIG_LV_FREETYPE_USE_LVGL_PORT=y` (FreeType uses LVGL memory/FS),
  `CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT=1024` (glyph cache count — sized for
  full pages of CJK text in the reader, so glyphs are not re-read from the SD
  font mid-page),
  `CONFIG_LV_DRAW_THREAD_STACK_SIZE=32768` (glyph rasterization runs on the
  LVGL draw thread; `lv_conf_internal.h` enforces >=32KB at compile time).
- **LVGL heap in PSRAM**: `CONFIG_LV_USE_CUSTOM_MALLOC=y` plugs in a custom
  `lv_malloc_core` (`components/graphics/ui/src/espaperplay_lv_mem_psram.c`)
  that places the entire LVGL heap (objects, render buffers, FreeType caches)
  in PSRAM; internal RAM is left to WiFi / Web / task stacks. (If you touch the
  allocator, keep the ui component's `WHOLE_ARCHIVE` — single-pass linking
  otherwise skips the archive member and the backend silently goes missing.)
- **Link hook** (`components/graphics/fonts/CMakeLists.txt`): when
  `LV_USE_FREETYPE` is on, the lvgl component compiles `src/libs/freetype/*.c`
  which needs the freetype headers and macros, but espressif/freetype is not
  linked into lvgl automatically — the fonts component runs
  `target_link_libraries(lvgl__lvgl PUBLIC idf::espressif__freetype)` and adds
  the global compile definitions `FT_ERR_PREFIX=FT_Err_` /
  `FT_CONFIG_OPTION_ERROR_STRINGS` / `FT2_BUILD_LIBRARY` (same approach as the
  official esp_lvgl_adapter).
- **LVGL filesystem port (critical)**: `LV_FREETYPE_USE_LVGL_PORT` works by
  LVGL's `lv_ftsystem.c` providing strong symbols with the same names as
  freetype's own `ftsystem.c` (`FT_Stream_Open` / `FT_New_Memory` /
  `FT_Done_Memory`, implemented over `lv_fs_open` / `lv_malloc`), overriding
  the ANSI fopen implementation at link time. If freetype's `ftsystem.c` is
  not removed from the freetype target, the linker pulls the ANSI version
  first and the LVGL version becomes dead code, so `fopen("A:xxx.ttf")` fails
  at runtime — the fonts component CMake removes `ftsystem.c` from the
  freetype target and compiles `lv_ftsystem.c` instead (this is what actually
  makes `LV_FREETYPE_USE_LVGL_PORT` take effect; symptom:
  `lv_freetype_font_create` returns NULL with a `FT_New_Face` error log).
- The FreeType engine is initialized automatically by `lv_init()` — no manual
  init call needed.
- **Renderer thread stack**: FreeType glyph rasterization (smooth renderer)
  needs a large stack. This project renders single-threaded
  (`LV_OS_NONE`, no LVGL draw thread), so rasterization runs inside the
  `gui_lvgl` task, whose stack was raised from 8KB to **32KB in PSRAM**
  (`lvgl_port.c`, via `xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)`) — at 8KB,
  rendering a FreeType font on the test screen triggers
  `stack overflow in task gui_lvgl`, and a 32KB internal-RAM stack fails with
  `ESP_ERR_NO_MEM` (WiFi/Web eat the internal heap); note that
  `CONFIG_LV_DRAW_THREAD_STACK_SIZE` has no effect under `LV_OS_NONE`.
- Load API: `espaperplay_fonts_load("NotoSansSC_Regular.ttf", 24, STYLE)` →
  `lv_font_t *` (wraps `lv_freetype_font_create()`, bitmap render mode, small
  cache keyed by file/size/style), ready for
  `lv_obj_set_style_text_font()`.
- Verification: the UI test screen (`screen_test.c`) renders its title with the
  FreeType font — Chinese glyphs displaying correctly proves the
  mmap-partition → LVGL FS drive → FreeType rasterization chain end to end.
- Size: full libfreetype costs ~+400KB flash.

---

<a id="zh"></a>
## 简体中文

### 只读字体资产（fonts 分区）

字体服务（`components/graphics/fonts`）把 TrueType 字体作为只读 flash 资产发布：

- `assets/fonts/` 存放字体文件。`NotoSansSC_Regular.ttf`（2.2MB）是 Noto Sans SC
  的子集，覆盖 GB2312 一二级（6763 个常用汉字）+ ASCII + 常用中文标点；
  重新生成：`tools/fonttools-venv/bin/python tools/prepare_font.py`
  （变量字体源文件下载到 `tools/font_src/`，已 gitignore）。
- 构建期由 `spiffs_create_partition_assets`（来自 `esp_mmap_assets` 组件）把目录
  打包为 `fonts` 分区（8MB，`spiffs` 子类型，见 `partitions.csv`）的 SPIFFS 镜像，
  并生成 `mmap_generate_fonts.h`（文件索引 + 校验和）到组件构建目录；
  `idf.py flash` 会自动烧写该分区镜像。
- 运行期 `espaperplay_fonts_init()`（在 LVGL 启动后调用）用 `esp_mmap_assets`
  映射分区——字体字节留在 flash，**零 RAM 拷贝**——并经 `esp_lv_fs` 注册为
  LVGL 文件系统盘符 `A:`。之后以 LVGL 路径打开字体，如
  `A:NotoSansSC_Regular.ttf`（`espaperplay_fonts_get_path()` 可组装路径）。

注意：字体文件名只允许 `[A-Za-z0-9_]`——连字符会破坏生成的 C 枚举名
（`MMAP_FONTS_*`）；且**长度不得超过 `CONFIG_MMAP_FILE_NAME_LENGTH`（默认 16，
本项目已设 32）**——超长文件名构建期仅打 Warn 并静默截断，运行时
`strcmp` 匹配失败导致字体打不开（症状：`lv_freetype_font_create` 返回 NULL，
且 LVGL 日志无 `FT_Stream_Open` 错误以外的线索）。

#### SD 卡完整字库（优先）

除 flash 内的裁剪子集外，插入并挂载 SD 卡后设备可改用卡内的**完整字库**
（flash `fonts` 分区只有 8MB 裁剪子集；完整 Noto Sans SC 约 10MB）：

- SD 卡挂载后，字体组件会**额外注册一个 LVGL 文件系统盘符** `B:`
 （`espaperplay_sd_fonts.c`），把 `fopen` 风格访问代理到 SD 目录
  `ESPaperPlay_FONTS_SD_DIR`（默认 `/sdcard/system/fonts/`，置于 SD 根目录
  `system/` 顶层下，固件数据与小说 / EPUB 等用户内容物理隔离）。注册盘符
  `B:` 只是纯回调注册，开机无卡也安全。
- `espaperplay_fonts_load(name, size, style)` 按次解析来源：
  1. SD 卡已挂载**且** `/sdcard/system/fonts/{name}` 存在 → 加载完整字库
    （盘符 `B:`），日志 `(SD full)`；
  2. 否则回退 flash 子集（盘符 `A:`），日志 `(Flash subset)`。
- 把 `NotoSansSC_Regular.ttf` 放进 `/sdcard/system/fonts/`，全设备即以完整
  CJK 覆盖渲染，替代 GB2312 子集。
- **Web 管理页字体管理**：WebUI 提供「字体管理」分区，可**上传**字体文件
 （`.ttf` / `.otf` / `.ttc`）到 SD 卡、**枚举**已有字体并**选用**当前字体
 （NVS 持久化）。所选字体在卡上存在即生效；不存在时自动回退出厂内置字体，
  渲染不中断。页面附 Noto Sans SC（Google Fonts）下载入口提示。
  字体选用**重启后生效**。

### FreeType 字体渲染

字体渲染基于 **LVGL 9.5 FreeType 封装**（`src/libs/freetype/`，仅封装层）+ 外部
[`espressif/freetype`](https://components.espressif.com/components/espressif/freetype)
组件（真正的 libfreetype 2.14.3，registry 依赖）：

- Kconfig（`sdkconfig.defaults`）：`CONFIG_LV_USE_FREETYPE=y`、
  `CONFIG_LV_FREETYPE_USE_LVGL_PORT=y`（FreeType 走 LVGL 内存/文件系统）、
  `CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT=1024`（字形缓存数——按阅读器整页
  CJK 文本规模设定，整页渲染不再从 SD 字库反复重读字形）、
  `CONFIG_LV_DRAW_THREAD_STACK_SIZE=32768`（字形栅格化在 LVGL draw 线程执行，
  `lv_conf_internal.h` 编译期强制 >=32KB）。
- **LVGL 内存后端整体迁 PSRAM**：`CONFIG_LV_USE_CUSTOM_MALLOC=y` 挂入自定义
  `lv_malloc_core`（`components/graphics/ui/src/espaperplay_lv_mem_psram.c`），
  LVGL 堆（对象 / 渲染缓冲 / FreeType 缓存）全部落在 PSRAM；内部 RAM 留给
  WiFi / Web / 任务栈。（改动分配器时注意保住 ui 组件的 `WHOLE_ARCHIVE`——
  单遍链接会跳过归档成员，后端静默失效。）
- **链接钩子**（`components/graphics/fonts/CMakeLists.txt`）：lvgl 组件编译
  `src/libs/freetype/*.c` 时需要 freetype 头文件与宏，但 espressif/freetype
  不会自动链接进 lvgl——由 fonts 组件执行
  `target_link_libraries(lvgl__lvgl PUBLIC idf::espressif__freetype)` 并追加
  `FT_ERR_PREFIX=FT_Err_` / `FT_CONFIG_OPTION_ERROR_STRINGS` / `FT2_BUILD_LIBRARY`
  全局编译定义（与官方 esp_lvgl_adapter 的做法一致）。
- **LVGL 文件系统移植（关键）**：`LV_FREETYPE_USE_LVGL_PORT` 的机制是 LVGL 的
  `lv_ftsystem.c` 提供与 freetype 自带 `ftsystem.c` 同名的强符号
  `FT_Stream_Open` / `FT_New_Memory` / `FT_Done_Memory`（内部走 `lv_fs_open` /
  `lv_malloc`），链接时覆盖 ANSI fopen 实现。若不清除 freetype 目标中的
  `ftsystem.c`，链接器先拉入 ANSI 版、LVGL 版成为死代码，运行时
  `fopen("A:xxx.ttf")` 必然失败——fonts 组件的 CMake 会从 freetype 目标剔除
  `ftsystem.c` 并编入 `lv_ftsystem.c`（即 `LV_FREETYPE_USE_LVGL_PORT` 生效的
  真正开关；症状：`lv_freetype_font_create` 返回 NULL，日志
  `FT_New_Face ... error`）。
- FreeType 引擎由 `lv_init()` 自动初始化（无需手动调用）。
- **渲染线程栈**：FreeType 字形栅格化（smooth 渲染器）消耗大栈。本项目是
  LVGL 单线程渲染模式（`LV_OS_NONE`，无独立 draw 线程），渲染发生在
  `gui_lvgl` 任务内，其栈已从 8KB 提升到 **32KB 且放在 PSRAM**
  （`lvgl_port.c` 用 `xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)`）——8KB 时渲染
  FreeType 字体会触发 `stack overflow in task gui_lvgl`，32KB 放内部 RAM 又会
  `xTaskCreate` 返回 `ESP_ERR_NO_MEM`（WiFi/Web 占用内部堆）；
  （注意 `CONFIG_LV_DRAW_THREAD_STACK_SIZE` 在 `LV_OS_NONE` 下不生效）。
- 加载 API：`espaperplay_fonts_load("NotoSansSC_Regular.ttf", 24, STYLE)` →
  `lv_font_t *`（内部走 `lv_freetype_font_create()`，位图渲染模式，按
  文件名/字号/样式做小型缓存），可直接
  `lv_obj_set_style_text_font()` 使用。
- 验证：UI 测试页（`screen_test.c`）标题用 FreeType 字体渲染
  「UI Test · FreeType 字体渲染正常」——中文正常显示即证明
  mmap 分区 → LVGL FS 盘符 → FreeType 栅格化链路可用。
- 体积：libfreetype 全量链接约 +400KB flash。
