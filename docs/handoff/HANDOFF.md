# HANDOFF — 当前交接（单文件滚动更新）

> 读法：新会话开头读这一份即可恢复上下文，读完再动手。稳定知识看 `AGENTS.md`，本文件只放**易变的当前状态**。
> 写法：每次会话结束前**覆盖**本文件。只写会改变下一步行为的内容；不写历史流水账。过期记忆比没有记忆更有害。

最后更新：2026-10-27

## 1. 做了什么

### 本轮：笔刷式文本选择（TDD，纯几何测试缝 + GUI 接线）

- 触发：用户请求「选取文章文本时，目前是矩形框选择。请实现笔刷式的文本选择方式。Use Skill: tdd」。
- 设计决策（AskUserQuestion 已确认）：①与矩形拖选**共存，`b` 键切换**；②测试缝取**纯几何单元**（无 GTK）；③半径可调，`[` / `]` 调小/调大。
- TDD 红→绿（`tests/test_sel_brush.cpp`，手写断言骨架，7 切片 13 断言全绿）：
  - `brush::seg_dist`：点到线段最短距离（垂足在段内 / 垂足段外取端点 / 零长段）。
  - `brush::brush_ranges`：字符中心（x 中点 + run 纵中线）到笔画折线距离 ≤ r → 连续字符区间 `[i0,i1)`；覆盖空路径/空 mids/横贯/两端截断/拐角双簇/竖向穿过/单击点/距离恰等 r。
- 新文件：
  - `src/sel_brush.{hpp,cpp}`：纯几何命中测试，不依赖 GTK。
  - `tests/test_sel_brush.cpp` + CMake `enable_testing`（`ctest` 名 `sel_brush`）；Makefile 新增 `test` 目标。
- GUI 接线（`src/caj_viewer.cpp`）：
  - App 状态：`brush_mode` / `brush_r`（显示像素，默认 8，范围 3–40）/ `brush_pts`（拖选中笔画，画布坐标）；落定态 `sel_is_brush` / `sel_r`（半径按**页宽比**存，缩放不变）/ `sel_path`（**归一化页面坐标**，经 `norm_from_canvas` 逆旋转 + clamp，语义与矩形 sel[] 一致）。
  - 事件：press 起笔 1 点；motion 采样间距 ≥2px 追加；release 单击（<5px）仍= 清除选区，落定时换算归一化路径 + 半径。扫描图像页 toast 不变。
  - `sel_spans(lay)` 加笔刷分支：路径换算到缩放后页面坐标，run 用「包围盒外扩半径」粗筛后再 `run_geom`，逐字符 mids 走 `brush::brush_ranges`；矩形分支行为原样。
  - `compose` 高亮：改为**水平相邻/重叠 span 合并**（x 相接 0.5px 容差 + y 带重叠则合并 x/y 取 min/max）——矩形模式视觉不变，笔刷一行多簇分开涂。
  - draw() 预览：笔刷拖选中画半透明圆头笔画带（PAL_BRASS @0.30，线宽 2r，单点画圆点）；矩形仍虚线框。
  - 快捷键：`b`/`B` toggle_brush（toast 提示）、`[` / `]` brush_adjust(∓2)；光标 PENCIL；`open_path` / `clear_selection` 重置笔刷选区字段。
- 验证：`make build` 零警告；`make test`（ctest 1/1）；`make verify`（KDH 基准 `--info` 三段分析，exit 0）；`make smoke`（EXIT=124 路径）全过。

## 2. 当前状态

- 构建：`make build`，`-Wall -Wextra` 零警告（含新测试目标）。
- 本轮改动：`src/sel_brush.{hpp,cpp}`、`tests/test_sel_brush.cpp`（新增）；`src/caj_viewer.cpp`、`CMakeLists.txt`、`Makefile`（修改）。**全部尚未 commit**（是否提交由用户决定）。
- 源码规模：`src/caj_core.cpp` 1714 / `src/caj_core.hpp` 145 / `src/caj_viewer.cpp` ≈2600 / `caj_viewer.py` 2030。
- 样本：仓库无 .caj；本机样本在 `~/下载/` 与 `~/下载/论文 /`（**后者目录名以空格结尾**，shell 命令记得引号）。格式回归基准：`数智时代…_钱建农.caj`（KDH/3 页）。
- 仓库状态：分支 `feature/annotation`，全部本地提交未 push（用户决定暂不 push、暂不合 main）。
- 笔刷选择属 C++ 版**有意新增**（Python 版没有），符合 AGENTS.md 禁令要求；若沉淀稳定知识可在 AGENTS.md GUI 约定补一句（未做，等用户确认效果后再补）。

## 3. 关键决策

- 记忆分三层：L1 会话交接（本文件，日级）、L2 自动记忆（周级）、L3 项目知识库（月级）。**当前只落地 L1**。
- 本文件形态：**单文件滚动更新**，每次会话结束前自动覆盖。AGENTS.md 只放稳定知识，保持零 churn。
- 「AGENTS.md 注入滞后」：会话内快照 + 编辑后刷新延迟；需要即时生效时以磁盘文件为准。
- 与 Python 版的行为差异必须是有意的，并补记进 AGENTS.md「禁令」一节。
- 仓库卫生：`.sysroot/` 与测试样本一律不入库。是否 git commit / push 由用户决定，agent 不擅自执行。
- **C++ 主线暂留本地分支**：用户明确「先不动，保持本地」。
- 笔刷半径按页宽比存、路径存归一化页面坐标：与矩形 sel[] 同语义，缩放/旋转后命中集合不变；旋转场景未实测但映射逻辑复用 `norm_from_canvas`，理论一致。
- 环境教训：RunCommand 里 `make test && make verify` 管道串联曾整体超时 630s，但两条各自单独跑均秒级通过——长链命令超时先拆开单跑再下结论。

## 4. 阻塞

- 无硬阻塞。
- 限制：笔刷选择的 GUI 交互（画笔迹、b/[/] 键、缩放后命中、旋转页上使用）**未经用户目视确认**，只过了构建/单测/回归/冒烟。
- 限制（历史遗留，沿用）：run 级微缩放排版修复、页面旋转、最近打开文件的 GUI 交互均未目视确认。

## 5. 下一步

1. **等用户目视确认笔刷选择效果**（含矩形模式回归观感：高亮合并绘制应与之前无异）；不顺手确认上一轮 run_hfit 排版修复（马波样本）。
2. 用户确认后决定是否 commit；如需沉淀，把「笔刷式选择（b/[/]）」与「run 级横向微缩放」两句补进 AGENTS.md GUI 约定。
3. 文档债（沿用）：`docs/MAP.md` 行号漂移（viewer 现约 2600）；`build_ui` CSS sheet 就地复制 `PAL_*` 十六进制未收敛。
4. 渲染提速剩余项（未获授权）：② 拆分 text_layer/bg_layer；③ PangoLayout 跨次缓存；⑥ probe_fonts/写 doc.pdf 移出主线程；⑦ run_geom 重复布局、prefetch ±1 页。
