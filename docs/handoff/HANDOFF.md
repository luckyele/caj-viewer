# HANDOFF — 当前交接（单文件滚动更新）

> 读法：新会话开头读这一份即可恢复上下文，读完再动手。稳定知识看 `AGENTS.md`，本文件只放**易变的当前状态**。
> 写法：每次会话结束前**覆盖**本文件。只写会改变下一步行为的内容；不写历史流水账。过期记忆比没有记忆更有害。

最后更新：2026-09-26

## 1. 做了什么

- 把 `caj_viewer.py`（2030 行，行为基准）完整移植为 C++/GTK3：`src/caj_core.{hpp,cpp}`（格式解析，不依赖 GTK）+ `src/caj_viewer.cpp`（GTK3 前端，单文件），并改为「启动先进入界面，再选定文件」。
- 写好 `AGENTS.md`（50 行，稳定知识层）并完成 **3 个新会话验证任务，结论 3/3 生效**：
  - 任务 1（构建行为）：fresh-context 子代理直接走 `cmake -DCAJ_SYSROOT=...`，全程 **零 sudo / 零 apt install**，且主动执行了 `touch src/*.cpp`（这条坑只写在 AGENTS.md 里）→ 规则确实被自动注入（`<always_applied_workspace_rules>`，不必自己读文件）。
  - 任务 2（KDH 只读调研）：子代理完整复述 `0x3C`/`0xFC`、相位众数、`endobj` 相邻差分投票、周期候选 `{6,4,8,12,16,32,64}`、阈值 `≥3`、回退 `{0x30,0x00,0x0D,0x20}`，并**主动区分「哪些来自 AGENTS.md、哪些来自源码」**。
  - 任务 3（新增功能）：子代理新增「最近打开文件」，未破坏启动顺序与空文档守卫。
- 验证过程挖出 AGENTS.md **一处事实错误**并已修正（第 34 行 KDH 条目）：原文把「周期候选失败」与「回退固定常数」错误绑定，实际是**周期失败只换下一个候选，只有投票票数 <3 才回退固定常数**。
- 本轮验证遗留的两项处置已定：工具栏页码**回退**成 `%d / %d`；「最近打开文件」功能**保留**。

## 2. 当前状态

- 构建：`build/caj-viewer` 存在且可用；`-Wall -Wextra` **零警告**（已复核）。
- 格式回归：`--info` 对基准样本输出 `4B 44 48 20` / `KDH ` / `KDH`，EXIT=0。
- 源码规模：`src/caj_core.cpp` 1685 / `src/caj_core.hpp` 137 / `src/caj_viewer.cpp` 2404 / `caj_viewer.py` 2030 / `AGENTS.md` 50 行。
- 验证样本只在 `~/下载/`（仓库根**已无** .caj）：基准样本 `数智时代休闲文明的构建：从大众观光到个性化体验的产业演进_钱建农.caj`（KDH，3 页）；另有 `数字政府治理——基于社会形态演变进程的考察_戴长征.caj`、`游戏旅游体验的形成机理研究——以《黑神话：悟空》为例_漆家进.caj`。路径含全角冒号，命令里必须加引号。
- 分支：`feature/annotation`（另有 `main`）。
- 任务 3 新增功能：`App::recent_path/load_recent/save_recent/add_recent`（[caj_viewer.cpp#L837-L894](file:///home/lucky/caj-viewer/src/caj_viewer.cpp#L837-L894)），落盘 `~/.local/share/caj-viewer/recent.json`（最多 5 条，`.tmp` + `rename` 原子写）；`draw_welcome` 画点选热区 `RecentHit`，`on_press_cb` 用 `if (!a->rd)` 作第一层守卫。测试数据已清理（该 json 当前不存在）。
- L1 交接链路已用 fresh-context 子代理验证通过（两轮）：子代理按 AGENTS.md 的指针读了本文件，并正确复述了状态与风险。
- 「AGENTS.md 注入滞后」已查清，**磁盘上不存在规则缓存文件**（`state.vscdb` 副本里搜不到规则正文；`.trae-cn` / `.config` / `.local/share` 全盘搜索只命中 AGENTS.md 自身、IDE 的 Local History 快照和日志）。真实机制是**会话内快照 + 编辑后的刷新延迟**：改完 AGENTS.md 后立刻新建的子代理会拿到旧快照（第一次验证就是这么中招的），等刷新后注入内容与磁盘逐字一致（第二轮验证已确认）。→ 属瞬态竞态、会自愈，无需清理任何文件；但**需要即时生效时以磁盘文件为准**。派生约定：**AGENTS.md 保持零 churn**，易变内容一律放本文件。

### 自带自动记忆（Trae memory）的写入条件 — 已摸清，未提交

实现全在原生 Rust 侧（`resources/app` 下 JS 零引用）：客户端 `libharness.so` 的 `harness/memory/src/{hook,llm_extractor,service}.rs` + `harness/core/src/stream_ext/handlers/memory_trigger.rs`；服务端 `libai_agent.so` 的 `.../ai-agent/src/domain/plan/memory.rs`（~1640 行）+ `handler/chat/handler.rs`。

链路：云侧 SSE 下发 `chat_memory_trigger`（`ChatMemoryTriggerEvent{chat_memory_scene, force_update}`）→ harness `memory_trigger.rs` 收事件 → hook（workflow `builtin:memory_runtime`）→ llm_extractor 组输入（`history_id_list / force_update / current_user_memory / current_project_memory / lastest_message_summaries`，拼写原文如此）→ `ExtractResult`(9) → 写 `session_memory_*.jsonl` / `topics.md` / `user_profile.md` / `project_memory.md`。

- 两条硬短路（"不写"）：`chat_memory_scene is unknown`、`history_id_list is empty`。
- 服务端二次门控：`fetch_topics_memory` 输出必须是 `new_memory{...}`，否则 `no_update, skip writing`。
- 配置门槛：`CoreMemoryConfiguration{enabled,force_enabled,cloud_memory_enabled,local_read_enabled,local_write_enabled}`、`LocalMemoryConfig`、`ChatMemoryWithHistoryConfig`；本机 `memory-config.json` **未生成**（配置在云侧）。
- **实测驱动源**：产物 jsonl 的 `compact_summary_meta.trigger="auto"`、`mode="async"` ⇒ 写入由**自动上下文压缩（auto compact）**驱动、异步落盘；`topics.md` 走另一条 topics 抽取流程。
- **对本项目是低频事件**：20260926 今日 renderer.log `compact` 计数 = 0（无 `compact_finish`）→ 本项目记忆目录**今日零写入**（最后写入仍是 20260919）；同日对照组 `-run-media-lucky-Learing…` 发生两次 auto compact → 连写 2 条（12:25 / 12:27）。

→ 决策影响：**Trae 自带记忆不能当可靠的 L2 载体**（写入时机取决于云侧 auto compact + 云侧配置，本地不可控、不可自证）。未钉死项：`chat_memory_scene` 的具体取值（scene 由云侧随事件下发，两个 `.so` 字符串池无变体字面量）。

### ⚠️ 风险：整个 C++ 版未进版本库

`git status` 事实：

- **untracked**：`AGENTS.md`、`CMakeLists.txt`、`bootstrap-deps.sh`、`src/caj_core.cpp`、`src/caj_viewer.cpp`。
- `src/` 下**只有** `src/caj_core.hpp` 被 tracked；`main` 分支的 `src/` 完全为空，也不含 `CMakeLists.txt` / `bootstrap-deps.sh` / `AGENTS.md`。
- 曾 tracked 的样本 `.caj` 及 sidecar PDF 已从工作区删除（`git status` 显示 ` D`）。

→ **一次 `git clean -f` 即全丢。** 在用户明确同意前不要 commit。

## 3. 关键决策

- 记忆分三层：L1 会话交接（本文件，日级，episodic）、L2 自动记忆（周级 semantic）、L3 项目知识库（月级 procedural）。**当前只落地 L1；L2/L3 未获授权。**
- 本文件形态：**单文件滚动更新**（不按日期分文件、不保留归档）；更新时机：**每次会话结束前自动覆盖**。
- AGENTS.md 只放稳定知识（构建/验证/结构/格式/GUI 约定/禁令）；易变状态一律进本文件，两边不重复。
- 与 Python 版的行为差异必须是有意的，并补记进 AGENTS.md「禁令」一节。
- 是否 git commit 由用户决定，agent 不擅自提交。

## 4. 阻塞

- 无硬阻塞。
- 限制（如实记录，勿当成已验证）：任务 3 的 GUI 交互（最近列表渲染、点击打开）在沙箱里只验到「跑满 timeout 不崩 + 落盘内容正确」，**未经目视确认**。

## 5. 下一步

1. 待用户拍板：是否把 C++ 版（`src/*.cpp`、`CMakeLists.txt`、`bootstrap-deps.sh`、`AGENTS.md`、本文件）纳入版本控制 —— 这是当前最大风险敞口。
2. 用户如需，可在真实 GUI 里目视确认「最近打开文件」。
3. L2 / L3 记忆层：等用户授权后再动。
