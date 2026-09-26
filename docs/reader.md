# Reader (TXT / EPUB) · 阅读器

[← README](../README.md) · [引擎结构、支持范围和资源上限](reader-engine.md)

## English

The reader presents TXT and EPUB through a shared chapter/block model. TXT uses
bounded streaming segments with UTF-8, BOM-marked UTF-16/32 and GB18030 decoding.
EPUB uses a validated ZIP index, EPUB 3 navigation or EPUB 2 NCX, spine order and
in-document anchors. Chapters outside the TOC remain in the reading flow.

The parser implements a reading-oriented subset of XHTML and CSS, including
headings, paragraphs, quotations, lists, linearized tables, ruby annotations,
alignment, indentation and style inheritance. JPEG and non-interlaced PNG images
are decoded into a bounded target buffer. It is not a browser or a fully conforming
EPUB reading system; see the [engine reference](reader-engine.md) for limitations.

A background worker prefetches EPUB chapters and writes parsed results to SD.
Interactive loads use a ready result or cache first, then parse synchronously on
a miss. Pagination is lazy and cached separately; a first layout pass still takes
time for font measurement. Versioned cache keys prevent stale layouts from being
used after a parser change.

The reader home displays a list of cover thumbnails, book titles and reading
progress. The reading toolbar provides TOC, page jump, font size, grayscale refresh
and return controls. Tap zones, swipes and the BOOT key turn pages. Reading history
stores the chapter and page within it. Illustration pages can request a GRAY4 refresh;
subsequent page turns return to black and white.

Run the actual C engines with host sanitizers:

```sh
python3 tools/reader/test_reader.py --sample '/path/to/book.epub'
```

## 简体中文

阅读器通过统一的“章节 → 内容块”模型呈现 TXT 与 EPUB。TXT 改为流式索引和
有界分段，支持 UTF-8、带 BOM 的 UTF-16/32，以及 GB18030。EPUB 根据校验后的
ZIP 索引、EPUB3 nav / EPUB2 NCX、spine 阅读顺序和文档内锚点组织章节；目录外的
正文与插图仍保留在阅读流中。

正文实现适合电子书阅读的 XHTML / CSS 子集，包括标题、段落、引用、列表、线性化
表格、Ruby 注音、对齐、缩进和样式继承。JPEG 与非交错 PNG 直接解码到尺寸受限的
目标缓冲。它不是浏览器，也不声明完整 EPUB 阅读系统一致性；详细边界见
[引擎说明](reader-engine.md)。

EPUB 后台任务预取章节并写入 SD 缓存。交互加载优先采用预取结果或缓存，未命中时
同步解析。分页惰性计算并单独缓存，首次字体测量和排版仍需要时间；缓存键包含
引擎版本，解析行为变化后不会误用旧排版。

阅读器主页以封面缩略图、书名和进度列表展示图书。阅读工具栏提供目录、页码跳转、
字号、灰度刷新和返回；支持点按、滑动和 BOOT 键翻页。历史按章节与章内页保存。
插图页可触发 GRAY4 刷新，后续翻页恢复黑白。

主机回归直接编译实际 C 引擎，启用 AddressSanitizer / UndefinedBehaviorSanitizer；
可用上方命令添加本地 EPUB 样本。最终交付还需要固件构建、烧录和实机阅读验证。
