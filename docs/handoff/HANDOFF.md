# HANDOFF — 当前交接（单文件滚动更新）

> 读法：新会话开头读这一份即可恢复上下文，读完再动手。稳定知识看 `AGENTS.md`，本文件只放**易变的当前状态**。
> 写法：每次会话结束前**覆盖**本文件。只写会改变下一步行为的内容；不写历史流水账。过期记忆比没有记忆更有害。

最后更新：2026-09-26

## 1. 做了什么

- 把 `caj_viewer.py`（2030 行，行为基准）完整移植为 C++/GTK3：`src/caj_core.{hpp,cpp}`（格式解析，不依赖 GTK）+ `src/caj_viewer.cpp`（GTK3 前端，单文件），并改为「启动先进入界面，再选定文件」。
- 写好 `AGENTS.md`（52 行，稳定知识层）并完成 **3 个新会话验证任务，结论 3/3 生效**：
  - 任务 1（构建行为）：fresh-context 子代理直接走 `cmake -DCAJ_SYSROOT=...`，全程 **零 sudo / 零 apt install**，且主动执行了 `touch src/*.cpp`（这条坑只写在 AGENTS.md 里）→ 规则确实被自动注入（`<always_applied_workspace_rules>`，不必自己读文件）。
  - 任务 2（KDH 只读调研）：子代理完整复述 `0x3C`/`0xFC`、相位众数、`endobj` 相邻差分投票、周期候选 `{6,4,8,12,16,32,64}`、阈值 `≥3`、回退 `{0x30,0x00,0x0D,0x20}`，并**主动区分「哪些来自 AGENTS.md、哪些来自源码」**。
  - 任务 3（新增功能）：子代理新增「最近打开文件」，未破坏启动顺序与空文档守卫。
- 验证过程挖出 AGENTS.md **一处事实错误**并已修正（第 34 行 KDH 条目）：原文把「周期候选失败」与「回退固定常数」错误绑定，实际是**周期失败只换下一个候选，只有投票票数 <3 才回退固定常数**。
- 本轮验证遗留的两项处置已定：工具栏页码**回退**成 `%d / %d`；「最近打开文件」功能**保留**。
- 查清 Trae 自带自动记忆的写入条件，写成 `docs/trae-memory-写入条件调研.md`（391 行）→ 结论「**不可作可靠 L2 载体**」，摘要见 §2。
- 产出代码地图 `docs/MAP.md`（272 行）：两文件共 4089 行的结构地图 + **11 类「改动接缝」索引**（加按钮 / 加快捷键 / 改调色板 / 加格式 / 改 KDH 恢复 / 改缓存格式 / 何时必须 `bump_rev()` / 加鼠标交互 / 新增持久化 / 启动序列 / 渲染子进程），把新会话的定位成本从「通读 4000 行」压到「查一节表格」。
- 把 C++ 版一次性纳入版本控制（提交 `6fa8d41`），并**清理了此前误入库的历史**（详见 §2「仓库瘦身」，`.git` 96MB → 304K）。
- 修复「老文档的 Python 视角」误导：`README.md` 重写为 C++/GTK3 主线视角（仓库构成 / 依赖 / `make` 构建 / `make verify`·`make smoke` 验证 / 快捷键 / 原理），`Makefile` 默认目标由 `deb` 改为 `build`，新增 `build` / `verify` / `smoke` / `run` / `check-sample`，`.deb` 系列保留但明确标注为 Python 参考实现遗留（打包产物改到 `dist/`，避免与 CMake 的 `build/` 相撞）。同步更新 `AGENTS.md` 构建节、`MAP.md` 文件清单与陷阱表、`.gitignore` 加 `dist/`。

## 2. 当前状态

- 构建：本轮重新 `cmake -S . -B build -DCAJ_SYSROOT=$PWD/.sysroot/root` + `cmake --build build -j`，`-Wall -Wextra` **零警告**。
- 格式回归：`--info` 对基准样本输出 `4B 44 48 20` / `'KDH '` / `KDH`，EXIT=0。
- GUI 冒烟：`GDK_BACKEND=x11 timeout 8 ./build/caj-viewer <样本>` → exit=124（跑满超时，无崩溃）。
- 源码规模：`src/caj_core.cpp` 1685 / `src/caj_core.hpp` 137 / `src/caj_viewer.cpp` 2404 / `caj_viewer.py` 2030 / `AGENTS.md` 54；docs：`MAP.md` 273 / `trae-memory-写入条件调研.md` 391 / 本文件；构建入口 `Makefile` 134 / `README.md` 122。
- 构建入口已收敛到 `make`：`make`(=`build`) / `make verify`（`--info` 格式回归）/ `make smoke`（8 秒 GUI 冒烟，**退出码 124 才算通过**）/ `make run`（`FILE=...`）。`SAMPLE` 可在命令行覆盖；样本缺失时 `check-sample` 会给出提示并退出 1，不会抛一堆无关错误。
- 验证样本只在 `~/下载/`（仓库**无** .caj）：基准样本 `数智时代休闲文明的构建：从大众观光到个性化体验的产业演进_钱建农.caj`（KDH，3 页）；另有 `数字政府治理——基于社会形态演变进程的考察_戴长征.caj`、`游戏旅游体验的形成机理研究——以《黑神话：悟空》为例_漆家进.caj`。路径含全角冒号，命令里必须加引号。
- 「最近打开文件」：`App::recent_path/load_recent/save_recent/add_recent`（[caj_viewer.cpp#L837-L894](file:///home/lucky/caj-viewer/src/caj_viewer.cpp#L837-L894)），落盘 `~/.local/share/caj-viewer/recent.json`（最多 5 条，`.tmp` + `rename` 原子写）；`draw_welcome` 画点选热区 `RecentHit`，`on_press_cb` 用 `if (!a->rd)` 作第一层守卫。测试数据已清理（该 json 当前不存在）。

### 仓库状态（2026-09-26 清理后）

- 分支 `feature/annotation`，**全部本地提交均未 push**（`origin` 停在 `d20b3ea`）。**用户已决定暂不 push、暂不合 `main`**（2026-09-26）。要查当前落后/领先多少，别信本文件里的数字，直接跑 `git log --oneline origin/main..HEAD`。
- 近期提交按内容认，别记哈希（当前提交的哈希会随 amend 变）：修 README/Makefile 的 Python 视角 → 交接层更新 → C++ 版入库；再往前 `674f9ec` / `6339c50` / `ef8a7c6` / `1d7044f` 是历史清理时一并改写的旧提交，哈希已稳定。
- 受版本控制的文件 **14 个**：`.gitignore`、`AGENTS.md`、`CMakeLists.txt`、`LICENSE`、`Makefile`、`README.md`、`bootstrap-deps.sh`、`caj_viewer.py`、`docs/{MAP.md,handoff/HANDOFF.md,trae-memory-写入条件调研.md}`、`src/{caj_core.cpp,caj_core.hpp,caj_viewer.cpp}`。
- 清理经过（可复现）：`179e7ed` 曾一次性入库 `.sysroot/`（**5351 文件 / 187MB**，含 `.deb` 二进制），`32c7de3` 曾入库基准样本 + sidecar（各 1.6MB）。两者都只在本地、未 push。处置 = `git rm -r --cached .sysroot` → 提交 `6fa8d41` → `git filter-branch --index-filter 'git rm -r --cached --ignore-unmatch .sysroot "*.caj" "*.cajviewer.pdf"' -- 1d7044f..HEAD` → 删 `refs/original/*` + `git reflog expire --expire=now --all` + `git gc --prune=now`。
- 结果：`.git` **96MB → 304K**；`git log --all -- .sysroot "*.caj"` 输出为空。`ef8a7c6` / `1d7044f` 哈希未变（它们没碰过这些路径）。
- 遗留小瑕：`674f9ec` 的提交信息仍写「c++ update sysroot」，但它现在实际只新增 `src/caj_core.hpp`。对象已 gc，要改正得再改写一次历史；不影响功能，暂留。

### 自带自动记忆（Trae memory）的写入条件 — 已摸清，报告已入库

实现全在原生 Rust 侧（`resources/app` 下 JS 零引用）：客户端 `libharness.so` 的 `harness/memory/src/{hook,llm_extractor,service}.rs` + `harness/core/src/stream_ext/handlers/memory_trigger.rs`；服务端 `libai_agent.so` 的 `.../ai-agent/src/domain/plan/memory.rs`（~1640 行）+ `handler/chat/handler.rs`。

链路：云侧 SSE 下发 `chat_memory_trigger`（`ChatMemoryTriggerEvent{chat_memory_scene, force_update}`）→ harness `memory_trigger.rs` 收事件 → hook（workflow `builtin:memory_runtime`）→ llm_extractor 组输入（`history_id_list / force_update / current_user_memory / current_project_memory / lastest_message_summaries`，拼写原文如此）→ `ExtractResult`(9) → 写 `session_memory_*.jsonl` / `topics.md` / `user_profile.md` / `project_memory.md`。

- 两条硬短路（"不写"）：`chat_memory_scene is unknown`、`history_id_list is empty`。
- 服务端二次门控：`fetch_topics_memory` 输出必须是 `new_memory{...}`，否则 `no_update, skip writing`。
- 配置门槛：`CoreMemoryConfiguration{enabled,force_enabled,cloud_memory_enabled,local_read_enabled,local_write_enabled}`、`LocalMemoryConfig`、`ChatMemoryWithHistoryConfig`；本机 `memory-config.json` **未生成**（配置在云侧）。
- **实测驱动源**：产物 jsonl 的 `compact_summary_meta.trigger="auto"`、`mode="async"` ⇒ 写入由**自动上下文压缩（auto compact）**驱动、异步落盘；`topics.md` 走另一条 topics 抽取流程。
- **对本项目是低频事件**：20260926 今日 `renderer.log` 的 `compact` 计数 = 0 → 本项目记忆目录**今日零写入**（最后写入仍是 20260919）；同日对照组 `-run-media-lucky-Learing…` 发生两次 auto compact → 连写 2 条（12:25 / 12:27）。

→ 决策影响：**Trae 自带记忆不能当可靠的 L2 载体**（写入时机取决于云侧 auto compact + 云侧配置，本地不可控、不可自证）。未钉死项：`chat_memory_scene` 的具体取值（scene 由云侧随事件下发，两个 `.so` 字符串池无变体字面量）。

## 3. 关键决策

- 记忆分三层：L1 会话交接（本文件，日级，episodic）、L2 自动记忆（周级 semantic）、L3 项目知识库（月级 procedural）。**当前只落地 L1；L2/L3 未获授权**（L2 的唯一现成候选已被判定不可靠）。
- 本文件形态：**单文件滚动更新**（不按日期分文件、不保留归档）；更新时机：**每次会话结束前自动覆盖**。
- AGENTS.md 只放稳定知识（构建/验证/结构/格式/GUI 约定/禁令）；易变状态一律进本文件，两边不重复。AGENTS.md 保持**零 churn**（改动对新会话有注入延迟，见下）。
- 「AGENTS.md 注入滞后」机制已查明：**磁盘上不存在规则缓存文件**，真实机制是**会话内快照 + 编辑后的刷新延迟**（改完立刻新建的子代理会拿到旧快照，刷新后逐字一致）→ 瞬态竞态、会自愈；**需要即时生效时以磁盘文件为准**。
- 与 Python 版的行为差异必须是有意的，并补记进 AGENTS.md「禁令」一节。
- 仓库卫生：`.sysroot/`（可由 `bootstrap-deps.sh` 重建）与测试样本**一律不入库**；`.gitignore` 已含 `.sysroot/`、`compile_commands.json`。
- **未推送的本地历史允许改写**（用于清理误入库内容）；**改写前必须确认未 push** —— 这是本次清理成立的前提。
- 是否 git commit / push 由用户决定，agent 不擅自执行。
- **C++ 主线暂留本地分支**：用户 2026-09-26 明确选择「先不动，保持本地」，不合 `main`、不 push。代价是机器故障即整体丢失——这是当前唯一高影响风险，且属用户已知选择，不要反复重提。

## 4. 阻塞

- 无硬阻塞。
- 限制（如实记录，勿当成已验证）：「最近打开文件」的 GUI 交互（最近列表渲染、点击打开）在沙箱里只验到「跑满 timeout 不崩 + 落盘内容正确」，**未经目视确认**。

## 5. 下一步

1. 文档债（**本轮已修**）：`README.md` / `Makefile` 的 Python 视角已改为 C++ 主线（见 §1）。剩余同类项：`build_ui` 的 CSS sheet 就地复制了一份 `PAL_*` 十六进制（`on_mark_color` 另硬编码 `#2C2821`）—— MAP §5.C 已标注为已知例外，未处理。
2. 用户如需，可在真实 GUI 里目视确认「最近打开文件」。
3. L2 / L3 记忆层：L2 需另选**可自证**的载体（脚本/命令优于文档），等用户授权后再动。
4. 若后续要给 C++ 主线做发行包，`Makefile` 的 `deb` 通道不能直接复用（它打的是 `caj_viewer.py`，且依赖未入库的 `packaging/`），需另写。
