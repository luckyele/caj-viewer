# AGENTS.md

CAJ / KDH 文献阅读器。`caj_viewer.py` 是已验证的行为基准，`src/` 下的 C++ 版是当前主线。

**会话交接**：动手前先读 `docs/handoff/HANDOFF.md`（易变的当前状态：做了什么 / 现状 / 决策 / 阻塞 / 下一步）；会话结束前**覆盖更新**它。本文件只放稳定知识，不要往里写当前进度。本文件由 IDE 在会话开始时注入，改动只对**新会话**生效；改完立刻新建的子代理可能仍拿到旧快照，需要即时生效时以磁盘文件为准。

## 构建

- **禁止 sudo / apt install**，环境没有 root。依赖走本地 sysroot。
- 首次或 `.sysroot/` 缺失时：`./bootstrap-deps.sh`（apt-get download + dpkg-deb -x 到 `.sysroot/root`，并修复悬空 .so 链接）。
- 构建：
  ```bash
  cmake -S . -B build -DCAJ_SYSROOT=$PWD/.sysroot/root && cmake --build build -j
  ```
  系统已装 `libgtk-3-dev` 时可省略 `-DCAJ_SYSROOT`。
- `make build` 是上面两条的封装（检测到 `.sysroot/root` 时自动带开关）；`make verify` / `make smoke` 即下一节的验证前两条。
- `-Wall -Wextra` 已开启，**必须保持零警告**。
- 改完源码如果 `cmake --build` 没有重新编译，先 `touch src/*.cpp` 再构建。
- `Makefile` 里的 `deb` / `install-local` / `install` 打包的是 **Python 参考实现**（`caj_viewer.py`），且依赖未入库的 `packaging/`，与 C++ 主线无关，不要用它验证 C++ 改动。

## 验证（每次改动后至少跑前两条）

- 格式回归：`./build/caj-viewer --info <file.caj>` → 应打印 `文件头(前4字节)` / `标识文本` / `格式类别` 三段分析，`--info` 不弹窗。
- 仓库不含测试样本。本机样本在 `~/下载/*.caj`，其中 `数智时代休闲文明的构建…_钱建农.caj` 是 KDH / 3 页的基准样本。（验证时记得给路径加引号，文件名含全角冒号。）
- GUI 冒烟：`GDK_BACKEND=x11 timeout 8 ./build/caj-viewer <file.caj>`；无参启动应先进界面再弹文件选择框。
- 首次打开会写 sidecar 缓存，二次打开应近瞬时；怀疑缓存脏就删掉 `<源文件>.cajviewer.pdf` 再试。

## 代码结构

- `src/caj_core.{hpp,cpp}`：格式分析、PDF 提取/重建、KDH 解密、HN/C8、sidecar 缓存、Renderer（后台渲染 + 页布局）。**不依赖 GTK**，可单独编译进测试。
- `src/caj_viewer.cpp`：GTK3 前端，单文件，含 App 状态与全部事件回调。
- `caj_viewer.py`：行为基准。改动格式解析逻辑前先对照它。

## 格式要点（最容易重复踩坑的地方）

- 魔数：`CAJ` / `HN` / `C8` / `%PDF` / `KDH` / `TEB`。
- CAJ：`0x14` 处 int32 → 偏移 → 该处 int32 即 `pdf_start`；从最后一个 `endobj` 倒推 body。
- KDH 2.00：`0x3C` 处 int32 指向尾部明文 XML；`0xFC` 起为循环 XOR 密文。密钥靠**字节频率分析 + `endobj` 相邻差分投票**恢复，具体是周期候选 `{6,4,8,12,16,32,64}` 逐个试、周期失败只是换下一个候选；只有投票票数 <3 时才回退固定常数 `{0x30,0x00,0x0D,0x20}`——不要改成硬编码密钥。KDH 走 `kdh_decrypt`，不走 `extract_pdf_bytes`。
- HN / C8：隐藏页图像（`\xff\xd8`）走 DCTDecode 直嵌；私有 JBig 经 `libjbigdec.so` 的 `jbigDecode` 解码，以 **1bpp packed + 垂直翻转 + `/Decode [1 0]`** 无损嵌入（等价 PIL 的 `"1;I"` 极性）。该 .so 运行时可选，缺失时不要崩溃。
- sidecar 缓存头 = `int64 mtime_ns + int64 size + fmt`，补齐到 64B，后面接 PDF。`mtime_ns` 必须复刻 Python 的 `int(st.st_mtime * 1e9)` 舍入，否则 Python 版写的缓存会读不中。

## GUI 约定

- 整页渲染结果缓存进 cairo surface，key = 页号 + dw + dh + `rev` + 旋转角；标注/选区变化时必须 `bump_rev()` 使其失效。
- 可视区高度 = 窗口高 − 44（页脚）；`top_y = (ch_eff - dh) / 2`（短页居中）或 `-scroll`。
- **页面旋转（C++ 版新增，Python 版没有）**：`rot_all`（全部页）+ `rot_page[页]`（单页）叠加，`eff_rot(n)` 取模 360。它是**视图级状态、仅本次会话**：换文档即 `open_path` 重置，不写 sidecar。实现层次是「`compose` 产出**未旋转**的 uw×uh 表面 → `rotate_surface` 在整页表面层旋转」，因此标注/选区/文字/插图一并跟随；鼠标坐标由 `norm_from_canvas` 做**逆旋转**映射回未旋转页面空间（标注坐标语义不变）。横向页按「旋转后宽度」定标。快捷键 `r` 旋转本页、`R` 旋转全部。
- **空文档状态必须安全**：`rd == nullptr` 时 `draw()` 走 `draw_welcome` 早退，所有事件回调都要有 `rd` / `cur_ready` 守卫。
- 启动流程固定为：建界面 → `gtk_widget_show_all` → `g_idle_add` 加载命令行文件或弹文件选择框。不要改成启动前先加载/先弹框。
- 颜色一律用 `PAL_*` 常量（`PAL_DESK` / `PAL_PANEL` / `PAL_PAPER` / `PAL_INK` / `PAL_BRASS` / `PAL_MUTED`），标注色用 `MARK_HEX[]`，不要就地写十六进制。
- 渲染子进程（pdftohtml / pdftoppm / pdffonts）必须带 GCancellable 看门狗，别留下僵尸进程。
- **后台渲染不轮询**（C++ 版有意差异）：`Renderer::get_async` 完成时经 `set_wakeup` 钩子（`open_path` 里注册）用 `g_main_context_invoke(nullptr, drain_render_cb, this)` 唤醒主循环，主线程**一次性** `drain()`。钩子会在后台渲染线程触发：实现须线程安全，且必须**先在锁内取 `wakeup_` 副本、解锁后再调用**（`~Renderer` 可能在解锁后析构对象）。不要改回 `g_timeout_add` 轮询。
- **位图页输出 PPM 而非 PNG**（C++ 版有意差异）：`render_bitmap` 调 `pdftoppm` 时**不指定格式开关** —— `pdftoppm` 没有 `-ppm` 选项，PPM(P6) 就是「不指定格式」时的默认输出，传 `-ppm` 会被当非法参数直接报错、不产文件。gdk-pixbuf 内置 PNM loader，无需新依赖。实测 120dpi 单页 1.7s → 0.3s。
- 页级 LRU 上限 `Renderer::max_ = 24`（位图页约 5.5MB/页 @120dpi）。

## 禁令

- 不新增第三方依赖、不用 sudo、不写系统目录。
- 与 Python 版的行为差异必须是有意的，并补记在本文件里。
