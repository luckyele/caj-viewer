# CAJ 阅读器

一个纯 Python 实现的中国知网 `.caj` 文档阅读器，支持格式分析、内嵌 PDF 提取与重建，并以 GUI 形式渲染阅读。

## 功能特性

- **格式分析**：解析 `.caj` 文件的魔数、格式类别、页数、大纲等元信息。
- **PDF 提取与重建**：从 CAJ 容器中读取内嵌 PDF 对象流，按文档真实页序重建目录（Catalog）/ 根 `Pages` 与 `xref` 表，产出可渲染的标准 PDF。
- **多格式支持**：
  - `CAJ` —— 知网私有容器，内嵌 PDF 流（主要支持）。
  - `%PDF` —— 实质为标准 PDF，直接使用。
  - `HN` / `C8` —— 另一类私有格式，JPEG 页直接嵌入，JBig 页通过 `libjbigdec.so` 解码为灰度图。
  - `KDH` / `TEB` —— 其他变体（仅格式识别）。
- **异步渲染**：基于 `pdftoppm` 后台线程渲染，主线程零阻塞；LRU 缓存 + 相邻页预取，翻页接近即时。
- **GUI 阅读**：tkinter 实现的“夜读书房”主题，深黛青墨色桌面 + 暖纸页卡 + 黄铜强调色，支持：
  - 首页 / 末页 / 上一页 / 下一页 导航
  - 放大 / 缩小
  - 鼠标滚轮连续滚动（页内纵向滚动 + 跨页翻页）
  - 大纲侧栏跳转
  - 底部黄铜“页垛”阅读进度条
  - 格式分析对话框

## 依赖

- Python 3
- `tkinter`（通常随 Python 发行）
- [Pillow](https://python-pillow.org/)
- [poppler-utils](https://poppler.freedesktop.org/)（提供 `pdftoppm` / `pdfinfo`）
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
make deb                     # 生成 build/caj-viewer_1.0.0_all.deb
sudo apt install ./build/caj-viewer_1.0.0_all.deb
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
```

### 快捷键

| 快捷键 | 功能 |
| --- | --- |
| `←` / `PageUp` | 上一页 |
| `→` / `PageDown` | 下一页 |
| `+` / `=` | 放大 |
| `-` | 缩小 |
| 鼠标滚轮 | 连续滚动（页内滚动到底自动翻页） |

## 工作原理

1. **格式分析**：`CAJParser` 读取文件前 4 字节魔数，识别 `CAJ` / `HN` / `C8` / `%PDF` 等格式。
2. **PDF 重建**：
   - 对 `CAJ` 格式：按 `0x14` 处的指针定位内嵌 PDF 流，扫描所有 `endobj` 边界，解析对象字典中的 `Type` / `Parent` / `Kids`，按 `/Kids` 顺序 DFS 收集叶子页得到真实文档页序，新建根 `Pages` 与 `Catalog` 并补齐 `xref` / `trailer`。
   - 对 `HN` / `C8` 格式：解析页信息表，将标准 JPEG 页直接嵌入；私有 JBig 页通过 `libjbigdec.so` 解码为灰度 JPEG 后嵌入；解码失败则插入带提示文字的占位页。
3. **渲染**：`Renderer` 将重建的 PDF 写入临时目录，调用 `pdftoppm` 异步生成页面 PNG，按 LRU 策略缓存并预取相邻页。
4. **GUI**：`Viewer` 在 tkinter 画布上以“纸卡浮于桌面”的形式展示页面，支持缩放、滚动、翻页与大纲跳转。

## 许可证

本项目基于 [MIT License](LICENSE) 发布。

## 致谢

- [caj2pdf](https://github.com/htlsmile/caj2pdf) —— HN/C8 私有 JBig 解码（`libjbigdec.so`）源自该项目（Hin-Tak Leung, 2020-2021）。
- [poppler](https://poppler.freedesktop.org/) —— 页面渲染后端。
