# CAJ 阅读器

中国知网 `.caj` / KDH 文献阅读器：解析文件格式、从容器中提取并重建内嵌 PDF，再以 GUI 渲染阅读。

**当前主线是 C++ / GTK3 实现**（`src/`）。`caj_viewer.py` 是移植前已验证的行为基准（纯 Python / tkinter），保留作格式解析的对照参考，不再是推荐的使用入口。

## 仓库构成

| 路径 | 说明 |
| --- | --- |
| `src/caj_core.{hpp,cpp}` | 格式分析、PDF 提取/重建、KDH 解密、HN/C8、sidecar 缓存、后台渲染与页布局。**不依赖 GTK** |
| `src/caj_viewer.cpp` | GTK3 前端，单文件，含 App 状态与全部事件回调 |
| `caj_viewer.py` | 行为基准。改动格式解析逻辑前先对照它 |
| `docs/MAP.md` | 代码地图：文件/函数索引 +「想改 X 该动哪几处」的改动接缝表 |
| `docs/handoff/HANDOFF.md` | 会话交接：易变的当前状态（做了什么 / 阻塞 / 下一步） |
| `AGENTS.md` | 构建与验证约定、格式要点、禁令 |
| `CMakeLists.txt` · `bootstrap-deps.sh` · `Makefile` | 构建入口 |

## 功能特性

- **格式分析**：解析魔数、格式类别、页数、大纲等元信息（`--info`，不弹窗）。
- **PDF 提取与重建**：从 CAJ 容器中读取内嵌 PDF 对象流，按文档真实页序重建目录（Catalog）/ 根 `Pages` 与 `xref` 表，产出可渲染的标准 PDF。
- **多格式支持**：
  - `CAJ` —— 知网私有容器，内嵌 PDF 流（主要支持）。
  - `KDH`（`KDH 2.00`）—— 新版知网下载的加密格式：多字节循环 XOR 加密的 PDF。程序按字节频率分析自动恢复密钥（无需固定密钥），解密结果同样走 sidecar 缓存。
  - `%PDF` —— 实质为标准 PDF，直接使用。
  - `HN` / `C8` —— 另一类私有格式，JPEG 页以 DCTDecode 直嵌，私有 JBig 页经 `libjbigdec.so` 解码。
  - `TEB` —— 其他变体（仅格式识别）。
- **文本/位图混合渲染**：`pdftohtml -xml` 提取每页文本版面（文字内容、字体/字号、插图位置），用 cairo / Pango 以真实文本绘制——本来是文本的页就是文本，任意倍缩放仍清晰锐利；纯扫描图片页自动回退 `pdftoppm` 位图。后台线程渲染不阻塞主线程，LRU 缓存 + 相邻页预取，翻页接近即时。
- **GUI 阅读**（GTK3，“夜读书房”主题：深黛青墨色桌面 + 暖纸页卡 + 黄铜强调色）：
  - 打开文件（工具栏「打开」/ `Ctrl+O`，在当前窗口切换文档；不带参数启动时先进界面再弹选择框）
  - 首页 / 上一页 / 下一页 / 末页导航，底部黄铜“页垛”阅读进度
  - 放大 / 缩小
  - 文字拖选复制（鼠标拖选 → 淡蓝高亮 → `Ctrl+C` 或右键复制，`Esc` / 单击清除）
  - 标记模式 + 四色高亮（黄 / 绿 / 蓝 / 红），标注持久化到源文件同目录的 `<源文件>.annotations.json`
  - 鼠标滚轮连续滚动（页内纵向滚动 + 跨页翻页；平滑滚动按累积量触发，避免一个手势翻出多页）
  - 目录侧栏跳转、格式信息对话框、最近打开文件

## 依赖

- C++17 编译器、CMake ≥ 3.16
- GTK3 开发包（`pkg-config` 名 `gtk+-3.0`、`pangocairo`）、zlib
- [poppler-utils](https://poppler.freedesktop.org/)（提供 `pdftoppm` / `pdftohtml` / `pdffonts`）
- 可选：`libjbigdec.so`（来自 [caj2pdf](https://github.com/htlsmile/caj2pdf)，用于解码 HN/C8 格式中的私有 JBig 图像；放置于可执行文件同目录、工作目录或 `~/.local/lib/` 即可被自动加载，缺失时不会崩溃）

## 构建

本机环境**没有 root，禁止 sudo / apt install**（详见 `AGENTS.md`）。系统未装 `libgtk-3-dev` 时，依赖走本地 sysroot：

```bash
./bootstrap-deps.sh     # 首次或 .sysroot/ 缺失时：把 GTK3 开发包解到 .sysroot/root
make                    # 等价 make build → 产出 build/caj-viewer
```

不用 Makefile 时：

```bash
cmake -S . -B build -DCAJ_SYSROOT=$PWD/.sysroot/root && cmake --build build -j
```

系统已装 `libgtk-3-dev` 时可省略 `-DCAJ_SYSROOT`。构建已开 `-Wall -Wextra`，**必须保持零警告**。

## 用法

```bash
./build/caj-viewer 文献.caj          # 打开阅读窗口
./build/caj-viewer 文献.caj --info   # 仅打印格式分析，不弹窗
./build/caj-viewer                   # 不带参数：先进界面，再弹文件选择框
```

### 验证

```bash
make verify    # 格式回归：对基准样本跑 --info，应打印三段分析
make smoke     # GUI 冒烟：8 秒后自动结束，退出码 124 视为通过
```

仓库**不含测试样本**。本机样本在 `~/下载/*.caj`，默认基准样本是 KDH / 3 页的
`数智时代休闲文明的构建：从大众观光到个性化体验的产业演进_钱建农.caj`（文件名含全角冒号，命令行里务必加引号）。
换样本：`make verify SAMPLE=/path/to/file.caj`。

首次打开会写 sidecar 缓存，二次打开应近瞬时；怀疑缓存脏就删掉 `<源文件>.cajviewer.pdf` 再试。

### 快捷键

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl+O` | 打开文件 |
| `←` / `PageUp` | 上一页 |
| `→` / `PageDown` | 下一页 |
| `+` / `=` | 放大 |
| `-` | 缩小 |
| 鼠标拖选 | 选择文字（标记模式下为高亮标注） |
| `Ctrl+C` | 复制选中文本 |
| `Esc` / 单击空白 | 清除选区 |
| 鼠标滚轮 | 连续滚动（页内滚动到底自动翻页） |

## 工作原理

1. **格式分析**：读文件头魔数，按 NUL+空格剥离后识别 `CAJ` / `KDH` / `HN` / `C8` / `%PDF` / `TEB`。
2. **PDF 重建**：
   - 对 `CAJ` 格式：按 `0x14` 处的指针定位内嵌 PDF 流，扫描所有 `endobj` 边界，解析对象字典中的 `Type` / `Parent` / `Kids`，按 `/Kids` 顺序 DFS 收集叶子页得到真实文档页序，新建根 `Pages` 与 `Catalog` 并补齐 `xref` / `trailer`。
   - 对 `KDH` 格式：按 `0x3C` 处指针定位尾部明文 XML，切出 `0xFC` 起的 XOR 加密体，按相位字节频率分析 + `endobj` 相邻差分投票自动恢复循环 XOR 密钥（周期候选 `6/4/8/12/16/32/64`），解密定位 `%PDF-` 并截去尾部杂讯。
   - 对 `HN` / `C8` 格式：解析页信息表，标准 JPEG 页直接嵌入；私有 JBig 页通过 `libjbigdec.so` 解码后以 1bpp packed + 垂直翻转 + `/Decode [1 0]` 无损嵌入（等价 PIL 的 `"1;I"` 极性）；解码失败则插入带提示文字的占位页。
3. **渲染**：重建的 PDF 写入临时目录，后台线程调用 `pdftohtml -xml` 提取每页文本版面（文字内容、字体/字号/颜色、插图位置与图像），以 cairo / Pango 真实文本项绘制；无文本的页（纯扫描件、HN/C8 图像页，整文档无字体时由 `pdffonts` 一次性判定）回退 `pdftoppm` 位图渲染。结果按 LRU 策略缓存并预取相邻页。渲染子进程带 GCancellable 看门狗，不留僵尸进程。
4. **Sidecar 缓存**：解析/重建结果缓存为源文件同目录的 `<源文件>.cajviewer.pdf`（头部 64 字节记录 `int64 mtime_ns + int64 size + fmt` 用于失效校验，后台线程异步写入），同一文件复开接近即时。缓存头与 Python 版**逐字节兼容**，两种实现可互读彼此的缓存。
5. **GUI**：GTK3 窗口内以“纸卡浮于桌面”的形式展示页面；整页渲染结果缓存进 cairo surface（key = 页号 + 尺寸 + `rev`），标注/选区变化时递增 `rev` 使其失效。`Ctrl+O` 或工具栏「打开」可在当前窗口切换文档，切换失败保留原文档。

## 关于 `make deb`（遗留，与主线无关）

`Makefile` 里的 `deb` / `install-local` / `install` / `uninstall` 目标打包的是 **Python 参考实现** `caj_viewer.py`，
与 C++ 主线无关；且它们依赖的 `packaging/` 目录（含 `DEBIAN/control`）**未纳入版本控制**，从干净克隆执行会直接失败。
C++ 主线目前没有打包目标，直接 `make` / `make build` 使用 `build/caj-viewer` 即可。

## 许可证

本项目基于 [MIT License](LICENSE) 发布。

## 致谢

- [caj2pdf](https://github.com/htlsmile/caj2pdf) —— HN/C8 私有 JBig 解码（`libjbigdec.so`）源自该项目（Hin-Tak Leung, 2020-2021）。
- [poppler](https://poppler.freedesktop.org/) —— 页面渲染后端。
