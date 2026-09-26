# CAJ 阅读器

一个纯 Python 实现的中国知网 `.caj` 文档阅读器，支持格式分析、内嵌 PDF 提取与重建，并以 GUI 形式渲染阅读。

## 功能特性

- **格式分析**：解析 `.caj` 文件的魔数、格式类别、页数、大纲等元信息。
- **PDF 提取与重建**：从 CAJ 容器中读取内嵌 PDF 对象流，按文档真实页序重建目录（Catalog）/ 根 `Pages` 与 `xref` 表，产出可渲染的标准 PDF。
- **多格式支持**：
  - `CAJ` —— 知网私有容器，内嵌 PDF 流（主要支持）。
  - `KDH`（`KDH 2.00`）—— 新版知网下载的加密格式：多字节循环 XOR 加密的 PDF。程序按字节频率分析自动恢复密钥（无需固定密钥），解密结果同样走 sidecar 缓存。
  - `%PDF` —— 实质为标准 PDF，直接使用。
  - `HN` / `C8` —— 另一类私有格式，JPEG 页直接嵌入，JBig 页通过 `libjbigdec.so` 解码为灰度图。
  - `TEB` —— 其他变体（仅格式识别）。
- **文本/位图混合渲染**：`pdftohtml` 提取每页文本版面（文字、字体、字号、插图位置），以画布真实文本项绘制——本来是文本的页就是文本，任意倍缩放仍清晰锐利；纯扫描图片页自动回退 `pdftoppm` 位图。后台线程渲染，主线程零阻塞；LRU 缓存 + 相邻页预取，翻页接近即时。
- **GUI 阅读**：tkinter 实现的“夜读书房”主题，深黛青墨色桌面 + 暖纸页卡 + 黄铜强调色，支持：
  - 打开文件（工具栏“打开”按钮 / `Ctrl+O`，在当前窗口切换文档；不带参数启动时自动弹出选择框）
  - 首页 / 末页 / 上一页 / 下一页 导航
  - 放大 / 缩小
  - 文字拖选复制（鼠标拖选 → 淡蓝高亮 → `Ctrl+C` 或右键复制，`Esc` / 单击清除）
  - 鼠标滚轮连续滚动（页内纵向滚动 + 跨页翻页）
  - 大纲侧栏跳转
  - 底部黄铜“页垛”阅读进度条
  - 格式分析对话框

## 依赖

- Python 3
- `tkinter`（通常随 Python 发行）
- [Pillow](https://python-pillow.org/)
- [poppler-utils](https://poppler.freedesktop.org/)（提供 `pdftoppm` / `pdftohtml` / `pdffonts` / `pdfinfo`）
- 可选：`libjbigdec.so`（来自 [caj2pdf](https://github.com/htlsmile/caj2pdf) 项目，用于解码 HN/C8 格式中的私有 JBig 图像；放置于脚本同目录或 `~/.local/lib/` 即可被自动加载）

## 安装

### Debian / Ubuntu

```bash
sudo apt-get install python3 python3-tk python3-pil poppler-utils
pip install Pillow
```

### 打包为 .deb 并安装（推荐）

仓库自带 `Makefile`，可以把整个程序打包成符合 FHS 规范的 `.deb`
安装包，安装后会在应用菜单出现"CAJ 阅读器"图标，可右键 `.caj` 文件
选择"打开方式 → CAJ 阅读器"，也可双击直接打开。

```bash
make deb                     # 生成 build/caj-viewer_1.0.1_all.deb
sudo apt install ./build/caj-viewer_1.0.1_all.deb
# 之后在终端直接：
caj-viewer /path/to/file.caj
# 或者从应用菜单启动 → 不带参数时弹出文件选择对话框
```

依赖（python3 / python3-tk / poppler-utils 等）由 `Depends:` 字段
自动声明，`apt` 安装时会一并拉取。

卸载：

```bash
sudo apt remove caj-viewer
```

### 解码 HN/C8 的 JBig 页（可选）

从 [caj2pdf](https://github.com/htlsmile/caj2pdf) 获取 `libjbigdec.so` 后：

```bash
cp libjbigdec.so ~/.local/lib/
```

## 用法

```bash
python3 caj_viewer.py 文献.caj            # 打印格式分析并打开阅读窗口
python3 caj_viewer.py 文献.caj --info     # 仅打印格式分析，不打开窗口
python3 caj_viewer.py                     # 不带参数时弹出文件选择对话框
```

### 快捷键

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl+O` | 打开文件 |
| `←` / `PageUp` | 上一页 |
| `→` / `PageDown` | 下一页 |
| `+` / `=` | 放大 |
| `-` | 缩小 |
| 鼠标拖选 | 选择文字（非标记模式，松开出现淡蓝高亮） |
| `Ctrl+C` | 复制选中文本 |
| `Esc` / 单击空白 | 清除选区 |
| 鼠标滚轮 | 连续滚动（页内滚动到底自动翻页） |

## 工作原理

1. **格式分析**：`CAJParser` 读取文件头魔数，按 NUL+空格剥离后识别 `CAJ` / `KDH` / `HN` / `C8` / `%PDF` 等格式。
2. **PDF 重建**：
   - 对 `CAJ` 格式：按 `0x14` 处的指针定位内嵌 PDF 流，扫描所有 `endobj` 边界，解析对象字典中的 `Type` / `Parent` / `Kids`，按 `/Kids` 顺序 DFS 收集叶子页得到真实文档页序，新建根 `Pages` 与 `Catalog` 并补齐 `xref` / `trailer`。
   - 对 `KDH` 格式：按 `0x3C` 处指针切出 `0xFC` 起的 XOR 加密体，按相位字节频率分析 + `endobj` 相邻差分投票自动恢复循环 XOR 密钥（周期自适应 6/4/8/12/16/32/64），解密定位 `%PDF-` 并截去尾部杂讯。
   - 对 `HN` / `C8` 格式：解析页信息表，将标准 JPEG 页直接嵌入；私有 JBig 页通过 `libjbigdec.so` 解码为灰度 JPEG 后嵌入；解码失败则插入带提示文字的占位页。
3. **渲染**：`Renderer` 将重建的 PDF 写入临时目录，后台线程调用 `pdftohtml -xml` 提取每页文本版面（文字内容、字体/字号/颜色、插图位置与图像），文字以 tkinter 画布真实文本项绘制、插图以图像项绘制；无文本的页（纯扫描件、HN/C8 图像页，整文档无字体时由 `pdffonts` 一次性判定）回退 `pdftoppm` 位图渲染。结果按 LRU 策略缓存并预取相邻页。
4. **Sidecar 缓存**：解析/重建结果缓存为源文件同目录的 `<源文件>.cajviewer.pdf`（头部 64 字节记录 mtime 与大小用于失效校验，后台线程异步写入），同一文件复开接近即时；"打开文件"切换文档同样命中缓存。
5. **GUI**：`Viewer` 在 tkinter 画布上以“纸卡浮于桌面”的形式展示页面，支持缩放、滚动、翻页与大纲跳转；`Ctrl+O` 或工具栏"打开"可在当前窗口切换文档，切换失败保留原文档。

## 许可证

本项目基于 [MIT License](LICENSE) 发布。

## 致谢

- [caj2pdf](https://github.com/htlsmile/caj2pdf) —— HN/C8 私有 JBig 解码（`libjbigdec.so`）源自该项目（Hin-Tak Leung, 2020-2021）。
- [poppler](https://poppler.freedesktop.org/) —— 页面渲染后端。
