# Reader (TXT / EPUB) · 阅读器

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

<a id="en"></a>
## English

### Reader (TXT / EPUB)

The reader application (`components/applications/reader` + `screen_reader` /
`screen_reader_home`) turns the device into an e-book reader:

- **Formats** — plain TXT and EPUB 2/3. EPUB parsing is memory-frugal: the ZIP
  central directory is parsed once and entries (chapters / images / fonts) are
  inflated on demand into PSRAM and freed after use; the book is never loaded
  whole. Chapters are split by the table of contents (toc.ncx / nav), so
  cover / colophon / appendix documents merge into the reading flow instead of
  becoming fake "chapters".
- **Block-model rendering** — XHTML is parsed in a single pass into a shared
  block model (headings, bold/italic, quotes, images); a CSS subset applies
  alignment, bold/italic and the CJK 2em paragraph indent. The same block
  pipeline serves TXT. Rendering happens on the LVGL thread with **zero parse
  work**: a prefetch worker (lower priority, own stack) inflates + parses the
  next chapter into ready slots, with parsed results cached to SD keyed by
  mtime/size.
- **Pagination** — lazy: the page opens immediately while total page count is
  computed in the background in ≤8 ms time slices. Page boundaries are cached
  to SD (`/sdcard/system/cache/reader/*.pag`, keyed by book fingerprint + font
  size + content area), so re-opening a book or returning to a swapped-out
  chapter is instant (measured: 33 s first count of a heavy chapter → 0 ms on
  re-open).
- **Reading UI** — bottom toolbar: TOC / page-number jump / font size / gray
  refresh; full-screen TOC panel (paged, auto-located to the current chapter);
  tap zones + edge swipes + BOOT key for paging; progress is stored as
  chapter + page-in-chapter so resume never re-counts prefix chapters.
  The reader home (`screen_reader_home`) lists reading history and the SD book
  shelf.
- **Images** — image blocks occupy their own page: JPEG decodes streamingly
  via a vendored TJpgDec (MCU-by-MCU), PNG decodes row-by-row via zlib
  (all color types / bit depths 1-16, palette + tRNS; peak memory ≈ target
  RGB565 buffer + 2 scanlines). Images are scaled to fit the content area and
  an automatic **GRAY4 full refresh** is triggered after rendering an
  illustration page (noticeably finer gray levels), switching back to BW on
  the next page turn. Configurable (default on) from the settings page or Web
  console.
- **Title** — the status bar shows the book title (EPUB `dc:title` / TXT file
  name).

---

<a id="zh"></a>
## 简体中文

### 阅读器（TXT / EPUB）

阅读器应用（`components/applications/reader` + `screen_reader` /
`screen_reader_home`）把设备变成电子书阅读器：

- **格式**——纯文本 TXT 与 EPUB 2/3。EPUB 解析极度省内存：ZIP 只解析
  中央目录，条目（章节 / 图片 / 字体）按需 inflate 到 PSRAM、用后释放，
  绝不整书载入。章节按目录（toc.ncx / nav）切分，封面 / 版权页 / 附录
  等目录外文档并入阅读流，不再变成假「章节」。
- **块模型渲染**——XHTML 单遍解析为统一块模型（标题层级、粗斜体、引用、
  图片），CSS 子集支持对齐、粗斜体与 CJK 段落 2em 首行缩进；TXT 走同一条
  块流水线。LVGL 线程渲染时**零解析开销**：预取 worker（更低优先级、独立
  栈）把下一章解压 + 解析进就绪槽，解析结果按 mtime/size 键缓存到 SD。
- **分页**——惰性：打开即显示，总页数后台按 ≤8ms 时间片统计。页边界
  缓存到 SD（`/sdcard/system/cache/reader/*.pag`，键含书指纹 + 字号 +
  内容区尺寸），重开书籍或回到被换出的章节即时完成（实测：重章节首次
  统计 33s → 二开 0ms）。
- **阅读界面**——底边栏：目录 / 页码跳转 / 字号 / 灰度刷新；全屏目录面板
 （分页显示，自动定位当前章）；点按区 + 边缘滑动 + BOOT 键翻页；进度按
  「章 + 章内页」存储，续读无需重数前缀章节。阅读器主页
 （`screen_reader_home`）列出阅读历史与 SD 卡书架。
- **图片**——图片块独占一页：JPEG 经自持 TJpgDec 逐 MCU 流式解码，PNG 经
  zlib 逐行流式解码（全颜色类型 / 1-16 位深、调色板 + tRNS；峰值内存 ≈
  目标 RGB565 缓冲 + 2 行扫描线）。图片等比缩放内接内容区，插图页渲染完
  自动触发一次 **GRAY4 全屏刷新**（灰阶明显更细腻），翻页恢复黑白。可在
  设置页或 Web 管理页开关（默认开）。
- **标题**——状态栏显示书名（EPUB `dc:title` / TXT 文件名）。
