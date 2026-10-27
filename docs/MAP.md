# MAP — 代码地图（改动接缝索引）

> 读法：**改代码前先查第 5 节「改动接缝」**，那里列出每类改动必改的每一处。第 3/4 节是模块清单（含行号），只在需要定位时查。
> 行号基于 `src/caj_core.cpp` 1685 行 / `src/caj_viewer.cpp` 2404 行（2026-09-26）。行号会漂移，**函数名是稳定锚点**。
> 稳定约定看 `AGENTS.md`，当前状态看 `docs/handoff/HANDOFF.md`，本文只解决「这个功能在哪 / 改它要动哪几处」。

## 1. 全局判据（30 秒建立心智模型）

- **两层，一条硬边界**：`caj_core.*` 是纯逻辑（格式解析 / PDF 重建 / 渲染后端 / 缓存），**不依赖 GTK**；`caj_viewer.cpp` 是唯一前端（GTK3 + Cairo + Pango），**单文件**，含全部 `App` 状态与事件回调。
- **判断改动落在哪层**：涉及「字节 / PDF / 页模型 / 子进程」→ core；涉及「窗口 / 控件 / 绘制 / 鼠标键盘」→ viewer。跨层只走 `caj_core.hpp` 的 8 个公开函数（见 §3.1）。
- **无框架、无信号总线**：所有 UI 事件都通过 `g_signal_connect*` 直达自由函数或 `App` 成员，没有中间层。找不到调用点时直接搜函数名。
- **`caj_viewer.py` 是行为基准**：改 core 里任何格式解析逻辑前，先对照 Python 版同名部分。因此**搜函数名时常会命中两处**（.cpp 与 .py），注意区分。

## 2. 文件清单

| 文件 | 行数 | 角色 |
|---|---|---|
| [src/caj_core.hpp](file:///home/lucky/caj-viewer/src/caj_core.hpp) | 137 | core 公开 API + 数据模型（`Info` / `Page` / `PageLayout` / `Renderer`） |
| [src/caj_core.cpp](file:///home/lucky/caj-viewer/src/caj_core.cpp) | 1685 | 格式解析、PDF 提取重建、KDH 解密、HN/C8、sidecar 缓存、渲染器 |
| [src/caj_viewer.cpp](file:///home/lucky/caj-viewer/src/caj_viewer.cpp) | 2404 | GTK3 前端：调色板、App 状态、绘制、事件、持久化、`main` |
| [caj_viewer.py](file:///home/lucky/caj-viewer/caj_viewer.py) | 2030 | 行为基准（tkinter） |
| [CMakeLists.txt](file:///home/lucky/caj-viewer/CMakeLists.txt) | 44 | 构建；`CAJ_SYSROOT` 开关；`-Wall -Wextra` |
| [bootstrap-deps.sh](file:///home/lucky/caj-viewer/bootstrap-deps.sh) | — | 无 root 拉 GTK3 到 `.sysroot/` |
| [Makefile](file:///home/lucky/caj-viewer/Makefile) | 134 | 构建入口：`build` / `verify` / `smoke` / `run`（默认目标 = `build`）；`deb` 系列为 Python 参考实现的遗留打包通道 |

## 3. core 层地图（`caj_core.cpp`）

### 3.1 公开 API（`caj_core.hpp`，全部在 `namespace caj`）

| 函数 | 头文件 | 实现 | 职责 |
|---|---|---|---|
| `caj_open` | L35 | L505–548 | 解析文件头 → `Info{magic, fmt, page_off, toc_off, toc_size}` |
| `caj_page_num` | L38 | L583–594 | 容器头声明的页数（`%PDF` 特例走 pdfinfo） |
| `caj_toc` | L41 | L596–629 | 大纲项（硬编码记录结构 `256s24s12s12si`，标题 gb18030 解码） |
| `pdf_page_count` | L44 | L550–582 | pdfinfo 优先，失败回退正则 |
| `analyze_text` | L47 | L631–700 | `--info` 与「格式信息」对话框共用的文本 |
| `prepare_doc` | L52 | L1320–1356 | **文档装载唯一入口**：缓存命中直返，否则按格式提取 + 异步写缓存 |
| `Renderer` 类 | L95–135 | L1514–1685 | 后台渲染线程 + LRU（10 页 / 120 dpi） |

### 3.2 内部区块

| 区块 | 行号 | 说明 |
|---|---|---|
| 基础工具 | L32–146 | `read_file` / `i32` / `u16` / `sha1_hex` / `uuid_hex` / `gb18030_to_utf8` |
| **子进程 `run_capture`** | L147–216 | **pdftohtml / pdftoppm / pdffonts / pdfinfo 的唯一入口**。GSubprocess + GCancellable 看门狗线程，超时 `force_exit`，无僵尸 |
| libjbigdec 动态加载 | L218–278 | `jbig_lib()`（L225）/ `jbig_decode_1bpp()`（L252，输出 1bpp packed + 垂直翻转） |
| HN/C8 占位页 | L280–393 | 解码失败时画提示页（`placeholder_rgb` L316） |
| 图像工具 | L395–481 | `zlib_deflate` / `jpeg_size` / `bitmap_from_pixbuf` / `load_bitmap_file` |
| 容器头解析 | L483–502 | `read_head4` / **`file_mtime_ns`（L495，舍入见 §5.F）** |
| 公开 API 实现 | L503–700 | §3.1 |
| 内嵌 PDF 提取重建 | L702–920 | `extract_pdf_bytes`（L707）—— CAJ 格式：`0x14` 取 `pdf_start`，`endobj` 倒推 + `/Kids` DFS 定页序 |
| HN / C8 构建 PDF | L922–1143 | `embed_from_rgb` / `hn_offsets` / `build_hn_pdf`（L978） |
| KDH 解密 | L1145–1243 | `xor_rep` / `kdh_pdf` / `kdh_try` / `kdh_decrypt`（详见 §5.E） |
| sidecar 缓存 | L1245–1316 | `cache_path_for` / `try_load_cache` / `save_cache`（详见 §5.F） |
| 渲染器 | L1358–1685 | `LayoutCtx` + GMarkup 解析 pdftohtml XML（L1362–1512）；`Renderer::*`（L1514 起） |

## 4. GUI 层地图（`caj_viewer.cpp`）

### 4.1 顶部常量与状态

| 项 | 行号 | 说明 |
|---|---|---|
| 调色板 `PAL_*` | L32–41 | DESK / PANEL / PANEL_EDGE / PAPER / PAGE_BG / SHADOW / INK / BRASS / BRASS_DIM / MUTED |
| 标注色 `MARK_NAME[]` / `MARK_HEX[]` | L59–60 | 4 项，**下标必须对齐** |
| `Annotation` | ~L578–587 | `id` / `created` + 几何 |
| `RecentHit` | L591–594 | 欢迎界面热区（画布坐标），`draw_welcome` 填充 |
| **`App`** | L597–734 | 全应用状态 + 方法声明。**加字段只改这里** |

`App` 字段分簇：文档（`path/cap/pdf/rd/N/cur/zoom/scroll`）· 交互（`annos/mark_on/dragging/selpress/has_sel/ptr_*`）· 缓存（`psurf/ps_*/zoom_preview/bmp_cache/bmp_holder`）· 几何（`cur_dw/cur_dh/cur_top/cur_ready`）· 控件（`win/canvas/page_lbl/title_lbl/toc_*/mark_btn`）。

### 4.2 渲染与绘制

| 函数 | 行号 | 职责 |
|---|---|---|
| `queue_draw` / **`bump_rev`** / `begin_zoom_preview` / `cancel_zoom_preview` / `zoom_preview_cb` | L662–683 | 缓存失效与缩放预览（详见 §5.G） |
| `display_scale` | L692–694 | `(max(cw,50) / img_w) * zoom` —— 铺满画布宽 |
| `compose` | L995–1131 | 把 `PageLayout`（文本项 + 插图）/ `Bitmap` 画进整页表面 |
| `bitmap_cached` | L981–993 | 位图表面缓存（key = `Bitmap*`） |
| **`page_surface`** | L1115–1131 | **整页表面缓存唯一入口**，key 含 `rev` |
| `draw` | L1327–1448 | 主绘制：桌面 → 早退（`!rd` → `draw_welcome`）→ 未就绪（`draw_placeholder`）→ 页影 → 页 → 拖选预览 → 页脚 → toast → `prefetch()` |
| `draw_welcome` | L1207–1325 | 空文档欢迎页（含 `recent_hits` 热区） |
| `draw_placeholder` | L1168–1205 | 「正在渲染」占位页 |
| `draw_footer` | L1133–1166 | 页脚/进度 |
| `toast_msg` | L906–924 | 轻提示（右下角浮出，靠 `toast` + `toast_tok`） |

> 竖向几何公式集中在 `draw`（L1352–1362）：`ch_eff = ch - 44`；`top_y = (dh <= ch_eff) ? (ch_eff-dh)/2 : -scroll`。**别在别处重算**，用 `page_rect`（L926）/ `norm_from_canvas`（L933）。

### 4.3 导航与渲染请求

| 函数 | 行号 | 职责 |
|---|---|---|
| `load_toc` / `toc_clear` / `cut_chars` | L1457–1480 | 目录侧栏填充 |
| `set_page` | L1482–1496 | **翻页唯一入口**：钳位 → 更页码 → 重置 `scroll` → `request_page` |
| `wheel_page` / `real_wheel` | L1498–1530 | 滚轮：页内滚动到底自动翻页；`flip_until` 是翻页后 0.15s 尾随事件保护 |
| `prefetch` | L1532–1539 | 预取相邻页（`draw` 末尾调用） |
| `request_page` | L1541–1548 | 经 `pending_page` 去重后投递异步渲染 |
| `on_page_ready` | L1550–1555 | 渲染完成回调 → `queue_draw` |
| `drain_render_cb` | L2283–2293 | 由渲染线程经 `g_main_context_invoke` 投递的一次性回调（`G_SOURCE_REMOVE`）：`rd->drain()` 取回结果。**已取代旧的 60ms `poll_render_cb` 轮询** |

### 4.4 文档打开与持久化

| 函数 | 行号 | 职责 |
|---|---|---|
| `open_file` | L1565–1591 | 文件选择对话框 → `open_path` |
| **`open_path`** | L1593–1655 | **打开/切换文档唯一入口**：新建临时目录 → `prepare_doc` → `new Renderer` → 换状态 → `load_annotations` → `add_recent` → 更新标题/目录 → `set_page(1)` → 删旧临时目录。失败时保留原文档 |
| `show_info` | L1657–1679 | 格式信息对话框（`!rd` 时 toast 提示） |
| `anno_path` / `load_annotations` / `save_annotations` | L737–850 | 标注持久化：优先 `<源文件>.annotations.json`，不可写退 `~/.local/share/caj-viewer/annotations/<sha1_16>.annotations.json` |
| `recent_path` / `load_recent` / `save_recent` / `add_recent` | L852–894 | 最近打开：`~/.local/share/caj-viewer/recent.json`，最多 5 条，`.tmp` + `rename` |

### 4.5 标注与文本选区

| 函数 | 行号 | 说明 |
|---|---|---|
| `anno_at` / `anno_menu` / `anno_action_cb` | L1681 / L1737 / L1704 | 标注命中测试 + 右键菜单 |
| `sel_spans` | L1756–1809 | 版面 → 选区片段（含 CJK 行首缩进与词间空格处理） |
| `sel_text` / `copy_selection` / `clear_selection` / `sel_menu` / `sel_action_cb` | L1811–1900 | 选区取文与复制 |

### 4.6 事件与 UI 构建

| 函数 | 行号 | 说明 |
|---|---|---|
| `update_cursor` | L1902–1910 | 光标形态（标记十字 / 文本 I 形 / 默认） |
| `on_draw_cb` | L1911–1916 | → `App::draw` |
| `on_press_cb` | L1917–1950 | **鼠标按下总分发**（`!rd` 守卫在 L1921） |
| `on_motion_cb` / `on_release_cb` | L1951 / L1960–2030 | 拖画 / 拖选结束（`bump_rev` 在 L2000 / L2026） |
| `on_scroll_cb` | L2031–2059 | 平滑滚动累加器 → `wheel_page` |
| `on_key_cb` | L2060–2102 | 快捷键 switch |
| `on_toc_row` / `on_toc_toggle` / `on_mark_toggle` / `on_mark_color` | L2104–2143 | 控件回调 |
| `open_initial_cb` / `choose_file_cb` / `on_destroy_cb` | L2158 / L2165 / L2171 | 启动与退出 |
| **`build_ui`** | L2314–2466 | **全部控件在这里创建**：CSS sheet → 工具栏 → 目录侧栏 → 画布 + 事件注册。**末尾不再建轮询定时器**（唤醒由 `Renderer::set_wakeup` 钩子投递 `drain_render_cb`） |
| **`main`** | L2346–2404 | 启动序列（详见 §5.J） |

## 5. 改动接缝（改之前先看这里）

### A. 加一个工具栏按钮

1. 在 `build_ui` 的 `navs[]`（L2240–2243，导航类）或 `tools[]`（L2251–2255，工具类）加一对 `{label, callback}`。
2. 数组类型是 `std::pair<const char*, void (*)(App*)>` —— 回调必须是**无捕获 lambda 退化成的函数指针**。若需要额外参数（如颜色值），不能进数组，改用 L2237 的写法：
   `g_signal_connect_swapped(b, "clicked", G_CALLBACK(+[](App* p){ ... }), a);`，或用 `g_object_set_data` 挂数据（见 L2281）。
3. 样式由 CSS sheet 的 `button` 规则统一给，**不要单独写样式**。

### B. 加一个快捷键

`on_key_cb`（L2060–2102）的 switch 加 case。带修饰键照 Ctrl+O / Ctrl+C 的写法（`ctrl` 取自 L2062 的 `GDK_CONTROL_MASK`）。**未命中必须落回 `return FALSE`**（L2101），否则吞掉其它快捷键。

### C. 改调色板

**三处，缺一不可**：

1. `PAL_*` 常量（L32–41）。
2. **`build_ui` 的 CSS sheet（L2183–2205）—— 这里把 `PAL_*` 的十六进制就地复制了一份**，改 `PAL_*` 不会带动 CSS。对应关系：`#232B31`=DESK、`#2E3942`=PANEL、`#3D4A54`=PANEL_EDGE、`#F6F1E4`=PAPER、`#C8A43A`=BRASS、`#2C2821`=INK、`#9AA69E`=MUTED、`#EFE8D7`=PAGE_BG。
3. `on_mark_color`（L2136–2140）里的 `color: #2C2821` 也是 `PAL_INK` 的硬编码副本。

标注色改 `MARK_HEX[]` + `MARK_NAME[]`（L59–60）；颜色菜单按 `i < 4` 同时索引两者（L2273–2284），**长度必须一致**。

> 这是 AGENTS.md「不要就地写十六进制」目前的已知例外。要彻底消除，得把 sheet 从 `static const char*` 改成运行时 `snprintf` 拼接 —— 属于待办，不是现状。

### D. 加一种格式支持

**5 处**：

1. `caj_open`（L505–548）加魔数分支，设 `page_off` / `toc_off` / `toc_size`（剥离规则见 L529–535）。
2. `prepare_doc` 的 dispatch 链（L1339–1349）加分支。
3. 写提取函数，仿 `build_hn_pdf`（L978）/ `kdh_decrypt`（L1221）/ `extract_pdf_bytes`（L707）。
4. `analyze_text` 的 fmt 分支链（L673–685）加展示。
5. `caj_page_num`（L583–594）**一般不用改** —— 有 `page_off` 就自动可用；只有像 `%PDF` 那样另立门户时才在 L584 加特例。

`caj_toc`（L596–629）只有在目录表与 `256s24s12s12si` 同构时才能复用。

### E. 改 KDH 密钥恢复

三层，**不要跳层改**：

1. `kdh_decrypt`（L1221–1243）：切加密体（`0x3C` 取尾界、`0xFC` 起）+ 周期候选循环 `{6,4,8,12,16,32,64}`（L1233）。
2. `kdh_try`（L1166–1219）：单周期求解。字节频率取 tops（L1169–1177）→ `endobj` 相邻差分投票（L1181–1196）→ **票数 ≥3 才信**（L1203）→ 否则按固定常数 `{0x30,0x00,0x0D,0x20}` 回退（L1210–1217）。
3. `kdh_pdf` / `xor_rep`（L1155 / L1148）：纯异或 + `%PDF-` 定位。

> 注意：**周期失败只是换下一个候选**，只有投票票数 <3 才回退固定常数。别把两者写成同一个分支。

### F. 改 sidecar 缓存格式

**四处必须同步**，否则新旧缓存互不命中：

1. `file_mtime_ns`（L495）—— 舍入必须复刻 Python 的 `int(st.st_mtime * 1e9)`。
2. `save_cache`（L1296–1316）写头：`ns(8) + size(8) + fmt`，补齐 64B（L1308），`.tmp` + `rename`。
3. `try_load_cache`（L1271–1294）比头：前 16 字节 memcmp（L1288）+ fmt 字符串。
4. `cache_path_for`（L1247–1269）路径：优先 `<源文件>.cajviewer.pdf`（写探测 L1250），源目录不可写退 `~/.cache/caj-viewer/<sha1_16>.cajviewer.pdf`。

`try_load_cache` 用 `data.find("%PDF-")` 定位正文（L1290），所以**头长度变化不会错位**；反之，头里若出现 `%PDF-` 字样会误判起点。

### G. 什么时候必须 `bump_rev()`

`psurf` 是整页表面缓存，key = `cur + dw + dh + rev + obj`（`page_surface` L1117–1118）。
`bump_rev()`（L663）= `rev++` + 取消缩放预览。

> **规则：凡是不改变 `dw`/`dh` 但会改变画面内容的改动，都必须 `bump_rev()`**，否则会拿到旧表面（画面不更新）。

现有调用点：缩放（L701–702）· 换文档 `open_path`（L1645）· 标注增删改（L1713）· 清除选区（L1866）· 拖选手势结束/清除（L2000 / L2026）。
**新增任何页内叠加物（新标注类型、搜索高亮、批注框）都要接进这条链。**

两个独立于 `psurf` 的缓存：

- `bmp_cache`（L643 / `bitmap_cached` L981）key 是 `Bitmap*` **指针**；靠 `bmp_holder`（L644）持有当前页，防止 `Bitmap` 被回收后地址复用导致串图。加新的位图来源时**必须同样持有**。
- `Renderer` 内部 LRU（`cache_`，容量 `max_ = 24`、`dpi_ = 120`）。`rd->get()` **只读缓存、绝不触发同步渲染**（hpp L103）。要新内容就走 `get_async` + `drain`。

### H. 加一种鼠标交互（手势）

1. `build_ui`（L2321–2329）加事件掩码 + `g_signal_connect`。
2. 分发进 `on_press_cb`（L1917）/ `on_motion_cb`（L1951）/ `on_release_cb`（L1960）。
3. **回调第一层守卫必须是 `!a->rd` 或 `cur_ready`** —— 空文档时全部事件都会进来（现成范例：L1921）。
4. 光标形态加进 `update_cursor`（L1902–1910）。
5. 坐标一律用 `norm_from_canvas`（L933）；需要页几何用 `page_rect`（L926）。

### I. 新增一份持久化数据（仿标注 / 最近打开）

1. 路径解析：仿 `anno_path()`（L737–755）—— **先探源文件同目录可写性**（写 `.cajviewer.write_probe` 再删），不可写退用户目录。
2. 写盘：仿 `save_recent()`（L871–894）—— `.tmp` + `rename` 原子替换。
3. 加载时机：`open_path`（L1646–1647 附近），与 `load_annotations` / `add_recent` 并列。

### J. 启动序列（**不要改顺序**）

`main`（L2346–2404）：

```
gtk_init_check(L2358) → 参数解析 → 文件校验(L2362-2374) → analyze_text 打印(L2375)
  → --info 早退(L2376) → App 构造(L2388) → load_recent(L2390)
  → build_ui(L2391) → show_all(L2392) → update_cursor(L2393)
  → g_idle_add(open_initial_cb | choose_file_cb)(L2394) → gtk_main(L2395)
  → 清理：bmp_cache / psurf / rd / tmp(L2397-2402)
```

**必须先 `show_all` 再 `g_idle_add`**，否则会先弹文件框再出界面。

### K. 渲染子进程

所有外部渲染命令（`pdftohtml` / `pdftoppm` / `pdffonts` / `pdfinfo`）统一走 `run_capture`（L154–216）。它已带 GCancellable 看门狗 + 超时 `force_exit`。**不要另起 `popen`/`system`**。

## 6. 一次「打开文档」的完整路径

```
用户点「打开」/ 命令行给路径
  └─ open_file (L1565) / open_initial_cb (L2158)
      └─ App::open_path (L1593)
          ├─ g_dir_make_tmp 建临时目录
          ├─ caj::prepare_doc (core L1320)
          │    ├─ caj_open    → Info（魔数 / 偏移）
          │    ├─ try_load_cache → 命中即返回（近瞬时）
          │    ├─ 未命中：build_hn_pdf | 原样 | kdh_decrypt | extract_pdf_bytes
          │    └─ 异步线程 save_cache
          ├─ new caj::Renderer(pdf, tmp) （core L1514）
          ├─ 换 App 状态 + bump_rev
          ├─ load_annotations / add_recent
          ├─ set_page(1) → request_page
          └─ rm_rf 旧临时目录
              ↓ 渲染线程完成后 set_wakeup → g_main_context_invoke
      drain_render_cb (L2283) → rd->drain() → on_page_ready (L1550) → queue_draw
              ↓
      on_draw_cb (L1911) → App::draw (L1327) → page_surface (L1115, 含 rev) → compose (L995)
```

## 7. 已知重复与陷阱速查

| 陷阱 | 位置 | 说明 |
|---|---|---|
| `PAL_*` 在 CSS 里被复制一份 | L32–41 ↔ L2183–2205 | 改调色板必须两边都改 |
| `#2C2821` 硬编码 | `on_mark_color` L2139 | 是 `PAL_INK` 的副本 |
| 整页表面缓存的 `rev` | `page_surface` L1117 | 忘 `bump_rev()` → 画面不更新（静默 bug） |
| 位图缓存用裸指针做 key | L643 / L644 | 必须靠 `bmp_holder` 持有，否则串图 |
| 渲染子进程只有一个入口 | `run_capture` L154 | 别绕过它，否则丢超时保护 |
| `%PDF-` 既是格式名也是缓存正文定位锚 | `caj_open` L534 / `try_load_cache` L1290 | 改缓存头时注意 |
| `.py` 与 `.cpp` 同名函数 | 全局 | 搜函数名会双命中，先确认文件 |
| 老文档里的 Python 视角 | `README.md` / `Makefile` | 已于 2026-09-26 改以 C++ 为主线，但 `caj_viewer.py` 仍是行为基准，描述格式逻辑时仍可能读到 Python 措辞 |
| `make deb` 是遗留通道 | [Makefile](file:///home/lucky/caj-viewer/Makefile) | `deb` / `install-local` 系列打包的仍是 `caj_viewer.py`，且依赖未入库的 `packaging/`；C++ 主线只走 `make build` |
