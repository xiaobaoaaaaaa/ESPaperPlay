# TXT / EPUB 阅读引擎

引擎针对 ESP32-S3、8 MB PSRAM 和 480×800 墨水屏设计。目标是按出版物阅读顺序可靠地呈现文本和插图，保持内存有界；不是浏览器，也不声称完整通过 EPUB 阅读系统一致性要求。格式参考 [EPUB 3.3](https://www.w3.org/TR/epub-33/) 与 [EPUB Reading Systems 3.3](https://www.w3.org/TR/epub-rs-33/)。

## 数据与所有权

公共门面 `espaperplay_reader.c` 保持“书 → 章节 → 文本/图片块”的 API，TXT 分段也使用章节接口。正文视图不直接接触 ZIP、XML 或字符编码。解析模块都接收显式上下文，可在主机上直接编译测试。

| 模块 | 职责 |
| --- | --- |
| `parser/reader_text` | 流式编码识别、解码、换行统一、原始字节偏移索引、按需分段装载 |
| `parser/reader_zip` | 两遍流式中央目录、精确名称分配、哈希查找、存储/deflate 解压与 CRC |
| `parser/reader_xml` | 有界词法扫描、命名空间局部名、属性、实体、UTF-16 XML、包内 URL |
| `parser/reader_package` | container、OPF manifest/spine、fallback、EPUB3 nav / EPUB2 NCX、锚点 |
| `parser/reader_css` | 多个外链和内联样式、有限 import、选择器匹配、继承和层叠 |
| `parser/reader_xhtml` | 顺序文本流、段落与结构、块样式、图片资源引用 |
| `parser/reader_image` | JPEG MCU / PNG 扫描行解码，直接采样到目标尺寸 |
| `espaperplay_reader_epub.c` | 会话、预取任务、驻留结果、图片单槽缓存、封面探测 |
| `parser/reader_cache.inc` | 会话私有的缓存序列化和边界校验 |

每个结果包独立拥有正文、块表和图片索引；采纳时转移所有权，关闭或失败时统一释放。EPUB 保留当前章和至多一个预取结果。图片缓存键包含图片编号和目标尺寸。

EPUB 的书锁覆盖解析、图片资源读取和关闭；锁顺序固定为“书锁 → 就绪槽锁”。预取请求包含会话代数，关闭后旧请求不能进入新会话。预取失败也发布完成状态，后台分页不会永远重试坏章，也不会把上一章误当作坏章内容。

交互加载优先读预取结果与磁盘缓存，未命中时同步解析；后台轮询在 worker 可用时异步解析，任务创建失败则同步降级。章节预取任务使用 24 KiB PSRAM 栈，封面任务使用 16 KiB PSRAM 栈；SDMMC 读写不需要禁用 Flash cache。缓存只由 worker 写入，缓存键包含引擎版本；旧版正文和分页缓存不会混用。缓存损坏时重新解析源文件。

## TXT

- 识别 UTF-8（有/无 BOM）、带 BOM 的 UTF-16 LE/BE、UTF-32 LE/BE。
- 无 BOM 时先严格尝试 UTF-8；发现非法序列后重新用 GB18030 建立索引，兼容常见 GBK 文本。这个判定是启发式的，无法保证识别所有混合编码或损坏文本。
- GB18030 双字节表和四字节连续区间来自 Python `gb18030` codec，由 `tools/reader/generate_gb18030.py` 可重现生成，放在 Flash 常量区；不把映射表复制到 RAM。
- 非法码元和 NUL 替换为 U+FFFD。CRLF / CR 统一为 LF；不把不同段落拼接成一个段落。
- 顺序扫描一次建立原始偏移索引，UTF-8 误判回退时额外扫描一次；之后只读取目标分段。优先在换行后约 48 KiB 分段，无换行长段在约 64 KiB 的完整码点边界切分。
- TXT 的“第 N / M 节”是内存分段，不是小说语义章节。文件上限 64 MiB，最多 4096 段；极端密集空行也会触发分段数量上限。
- `get_text` 现在返回当前分段，不能用于取得整本 TXT。旧页码缓存按引擎版本失效；旧阅读历史按原有页码恢复，排版改变后落点可能有偏移。

## EPUB 支持范围

### 容器、书目、目录

- ZIP stored / raw deflate，支持 data descriptor。检查目录和本地头边界、名称一致性、实际解压长度、CRC、重复条目。条目名区分大小写。
- container 中选择本地 OPF；OPF 支持命名空间前缀、不同属性空白和引号、标题实体、manifest idref、spine 顺序和有限 fallback 链。未知 spine 类型无可用 XHTML/SVG fallback 时明确拒绝，不静默丢弃正文。
- EPUB3 `nav[epub:type~=toc]` 优先，缺失时使用 NCX，再缺失时每个 spine 文档作为一章。目录标签支持嵌套元素。
- URL 相对**引用所在文档**解析，包括百分号编码、Unicode 路径和锚点；拒绝网络资源、包外路径穿越和截断。先拆 fragment 再解码，编码后的 `%23` 文件名不被误当锚点。
- 同一文档的多个锚点分别成章。锚点按文档扫描解析一次，章节边界按 spine 和文档偏移排序；重复边界合并，无效锚点忽略。目录以外的插图/正文文档保留在相邻阅读区间中。
- 为了保留注释等辅助内容，`linear="no"` 文档仍纳入顺序阅读。未在 spine 内的任意超链接目标不会自动加入阅读流；不提供链接跳转、脚注弹窗或媒体叠加播放。
- 封面优先 EPUB3 `cover-image`、EPUB2 cover 元数据，再查首文档图片和常见 cover 文件名。封面使用独立 ZIP/书目上下文，不改当前阅读状态。

### 正文和样式

- UTF-8 / UTF-16 XML，CDATA、注释、数字实体、XML 实体及常见 HTML 出版实体。不读取外部 DTD；未知命名实体保留原文。
- 段落、h1–h6、引用、列表编号、定义列表、图注、预格式空白和换行；表格按行、单元格分隔符线性化。Ruby 保留基础文本和括号内注音，忽略 `rp` 的兼容括号。
- 所有本地 `link rel=stylesheet` 与 `style` 按文档顺序处理；有界 `@import`；不同文档的样式不串用。支持元素、类、id、组合、后代和直接子选择器，选择器优先级、行内样式和 `!important`。
- 属性子集：块级粗体/斜体、左右/居中对齐、段首缩进、`display:none`、预格式空白。正缩进统一为两个全角空格；不精确还原任意 CSS 长度单位。
- 当前渲染块只有一种字体，**不还原段内局部强调**。保留文字，但不会因为一个粗体 span 把整个段落改成粗体。
- 字体使用设备字库；不加载内嵌字体，不执行 JavaScript，不请求网络。颜色、绝对定位、浮动、flex/grid、复杂选择器、竖排、固定版式精确定位和交互表单不还原。MathML / SVG 文本按可读文本降级，不进行公式或矢量绘制。
- 原生支持 JPEG（TJpgD 支持的基线编码）与非交错 PNG（灰度、RGB、调色板、灰度透明、RGBA，合法 1–16 位深）。支持 XHTML 内 SVG 的 `image href/xlink:href` 位图包装。
- 不支持的图片资源保留 alt 或图片占位；渐进 JPEG、GIF/WebP、交错 PNG 和复杂 SVG 不保证显示。解码尺寸包含在缓存键中，输出不超过请求框。

### 资源预算

| 项目 | 上限/策略 |
| --- | --- |
| ZIP 条目 / OPF manifest / spine | 各 4096 |
| ZIP 名称总量 | 512 KiB；按实际量分配 |
| 解压后的单个资源 | 4 MiB |
| 单个 XML / 合并章节正文 | 2 MiB；UTF-16 转换后也检查 |
| 单章内容块 / 图片引用 | 20000 / 256 |
| XML 内容嵌套 | 64 层 |
| 目录 | 4095 条有效输入，可补一个前导章节 |
| CSS | 单资源 64 KiB，256 条规则；最多加载 16 个资源、import 深度 4、media 嵌套 4 |
| 图像源尺寸 | PNG 8000×8000；JPEG 16000×16000；接受阅读视图的双倍采样预算，目标每边最多 1600，RGB565 像素缓冲最多 2 MiB |

正文/容器超限或损坏返回错误，不能当作完整章节继续。CSS 超预算会忽略后续规则，保留默认样式下的正文。图片不支持时由阅读视图显示占位。ZIP64、多磁盘 ZIP、加密 ZIP、DRM 不支持。

## 回归方法

```sh
python3 tools/reader/test_reader.py
python3 tools/reader/test_reader.py --sample '/path/to/book.epub'
```

脚本用 Clang 的 AddressSanitizer / UndefinedBehaviorSanitizer 编译**实际 C 解析器和 EPUB 门面**，不是用 Python 重写一份解析器。合成测试涵盖编码、UTF 边界、长行、多分段、目录/锚点、CSS 覆盖、结构、图片、ZIP 损坏、资源上限和分配失败清理。每个进程检查解析器分配余额归零。测试样本不写入仓库；`--sample` 使用用户本地 EPUB 并解码各章插图。

主机时间与分配峰值不代表设备的总 RAM 或耗时；最终还需固件构建、烧录和实机翻页验证。修改公开资源上限、章节切分或正文块格式时，同步更新引擎版本和缓存版本。
