# 技术报告：Trae CN 自带自动记忆的写入条件

| 项 | 内容 |
|---|---|
| 调研对象 | Trae CN 自带「自动记忆」(`~/.trae-cn/memory/`) |
| 目的 | 摸清该机制**何时写、写什么、写不写由谁决定**，评估其能否作为本项目的 L2 语义记忆载体 |
| 方法 | 静态二进制考古（`.so` 字符串池 + 字节偏移定位源码路径与日志文案）+ 本机磁盘实证 + 日志统计 |
| 结论摘要 | 写入**由云侧 SSE 事件 + 自动上下文压缩驱动**，本地不可控、不可自证；**不建议作为 L2 载体** |
| 日期 | 2026-09-26 |
| 状态 | 只读调研，未修改任何被测系统文件 |

---

## 1. 结论摘要

1. **写入入口不在本地。** 触发源是云侧下发的 SSE 事件 `chat_memory_trigger`，本地 harness 只负责接收并转交。
2. **落盘的直接驱动是「自动上下文压缩（auto compact）」，且异步。** 产物每行都带 `compact_summary_meta.trigger="auto"`、`mode="async"`。**不是每轮对话都写。**
3. **有四道门控**，任一道不过即静默跳过（只写日志，不落盘）。
4. **配置在云侧。** 本机 `memory-config.json` 从未生成，本地配置日志中 `core_memory|chat_memory|local_memory` 零命中。
5. **对短/中任务属低频事件。** 本项目当日 `compact` 计数 = 0 ⇒ 记忆目录当日零写入。
6. **决策影响：不能作为可靠的 L2 载体。** 违反「一条命令可自证」原则。

---

## 2. 调研方法（可复现）

| 手段 | 说明 |
|---|---|
| `grep -abo "<pattern>" <file>` | 在 `.so` 中取**字节偏移**；`strings \| grep` 无法定位，必须用偏移 |
| `dd if=<file> bs=1 skip=$((off-N)) count=M \| strings` | dump 目标偏移**邻近的字符串池**，这是本轮取得突破的关键手段 |
| 磁盘实证 | `ls -la ~/.trae-cn/memory/` 逐项目比对产物与 mtime |
| 日志统计 | 对 `renderer.log` 统计 `compact` / `compact_finish` 出现次数 |
| 负向取证 | 对 `resources/app` 全量 JS/TS/JSON 反查 `memory_scene\|memoryScene\|chatMemoryScene`，**零命中** ⇒ 排除 JS 侧参与 |

**踩坑记录（后续复现时避免）：**

- `.so` 内字符串被非打印字节分隔，`strings | grep -B/-A` 的上下文窗口**无效**，必须改偏移定位。
- `strings` 输出经管道易触发 `grep: write error: 断开的管道`，需 `head -N` 限流、分段 dump。
- **截断名字表是主要噪声源**：16 字节截断的符号表会产生 `chat_memory_trigwrapped`、`message_referenc`、`hash_agent_revie` 这类看似有意义的"字符串"，dump 邻域后才能识别为噪声并排除。
- 其它已排除的误判：`Scene` 命中实为 Go EXIF 噪声；`chat_turn` 实为 DB 表名；`compact_trigger` 实为配置键 `v3_micro_compact_trigger_token_ratio`。

---

## 3. 架构分层与证据定位

实现**全部在原生 Rust 侧**，`resources/app` 下 JS 零引用。

### 3.1 客户端 `libharness.so`（196,922,368 B）

| 源码路径 / 模块 | 职责 | 关键字段与日志文案 |
|---|---|---|
| `harness/core/src/stream_ext/handlers/memory_trigger.rs`（:59） | 接收云侧 SSE 事件、触发 hook | `scene`；`cloud SSE chat_memory_trigger received`、`failed to fire memory trigger hook` |
| `harness/memory/src/hook.rs` | 触发钩子 | `message / session_id / scene / timeout_secs`；`builtin:memory_runtime`、`has_memory_context`、`memory context resolved` |
| `harness/memory/src/llm_extractor.rs` | 组 LLM 输入、解析输出 | 见 §5.2 |
| `harness/memory/src/service.rs` | 路径模板与配置读写 | `%Y%m%d`、`memory-config.json`、错误变体族（见 §6.3） |

**事件名枚举（同处字符串池）**：`task_created` / `thought` / `tool_call` / `revoke` / `turn_completion` / `progress_notice` / `missing_history` / `history` / `user_message` / `obtain_context` / `model_config` / `tool_cache_data` / **`compact` / `compact_finish` / `compact_token_usage`** / **`chat_memory_trigger`** / `queuing` / `token_usage` / `context_usage` / `timing_cost` …

> 注意 `compact*` 与 `chat_memory_trigger` 同属该枚举 —— 与 §7 的实测驱动源互为印证。

### 3.2 服务端 `libai_agent.so`（259 MB）

| 源码路径 | 职责 | 关键文案 |
|---|---|---|
| `apps/icube_server_rs/modules/ai-agent/src/domain/plan/memory.rs`（约 1640 行） | 记忆主体逻辑 | `fetch_topics_memory: no_update, skip writing` |
| `.../src/handler/chat/handler.rs` | 事件处理 | `handle_chat_memory_trigger_event: scene=, force_update=`、`Unknown chat_memory_scene:` |
| `.../src/domain/hub/hook/hooks/on_turn_end.rs` | 轮次结束钩子 | `hub_on_turn_end` |
| `.../src/domain/plan/context_obtainer/post_compact_restore.rs` | 压缩后上下文恢复 | — |
| `harness/server/src/modules/context_asset/handlers.rs` | 配置读写命令 | `memory.get_config` / `memory.set_config` / `memory.list` / `memory.delete` |

---

## 4. 触发链路

```
云侧 SSE 下发 chat_memory_trigger
  事件体：ChatMemoryTriggerEvent { chat_memory_scene, force_update }
    │
    ├─[客户端] memory_trigger.rs        收事件（日志 cloud SSE chat_memory_trigger received）
    │            └─ hook.rs             触发 hook，workflow = builtin:memory_runtime
    │                 └─ llm_extractor.rs  组装输入 → 调用 workflow/start
    │                       └─ ExtractResult(9) → 写产物
    │
    └─[服务端] handler.rs               handle_chat_memory_trigger_event
                 └─ memory.rs           二次门控 fetch_topics_memory
```

### 4.1 事件结构

`ChatMemoryTriggerEvent` 为 **2 elements**：

- `chat_memory_scene`
- `force_update`

`chat_memory_scene` 是**云侧运行时字符串**，harness 仅透传，两个 `.so` 的字符串池中**均无变体字面量** ⇒ 客户端无法枚举其取值。

### 4.2 抽取器输入字段（`llm_extractor.rs`）

```
message / workflow_name / history_count / conversation_id / workflow_session_id
current_user_memory / current_project_memory
latest_message_summaries / lastest_message_summaries   ← 原文拼写错误，两个都在
history_id_list / force_update / user_input / has_user_input / summarize_message
```

### 4.3 抽取器输出 `ExtractResult`（9 elements）

```
base_path / server_name / topics / session_summary
update_profile / user_profile_changed / project_memory_changed
user_profile / project_memory / conversation_topics / topic
```

---

## 5. 四道门控（"不写"的判定条件）

| # | 闸门 | 位置 | 不过时的表现 |
|---|---|---|---|
| 1 | 云侧必须下发 `chat_memory_trigger` | 云侧 | 本地无任何自触发路径 |
| 2 | `chat_memory_scene` 必须已知 | 客户端 `llm_extractor.rs` | `memory extraction skipped because chat_memory_scene is unknown` |
| 3 | `history_id_list` 必须非空 | 客户端 `llm_extractor.rs` | `memory extraction skipped because history_id_list is empty` |
| 4 | `fetch_topics_memory` 输出必须是 `new_memory{...}` | 服务端 `memory.rs` | `[fetch_topics_memory] empty history_ids, session_id=` ／ `fetch_topics_memory: no_update, skip writing` |

第 4 道的服务端入口日志：

```
[fetch_topics_memory] invoked: invocation_id=, session_id=, force_update=, history_ids_count=
```

配套函数 `extract_topics_and_update_memory`。

---

## 6. 配置族

### 6.1 核心结构体

```
CoreMemoryConfiguration {
    enabled, force_enabled, cloud_memory_enabled,
    local_read_enabled, local_write_enabled
}
LocalMemoryConfig { ..., hit_my_space }
ChatMemoryConfig
ChatMemoryWithHistoryConfig
ShallowMementoConfiguration
```

### 6.2 配置键

- 短名：`core_memory` / `chat_memory` / `local_memory` / `chat_memory_with_history` / `shallow_memento` / `past_chats` / `code_as_tool`
- 服务端开关：`chat_memory_enabled`、`chat_memory_local_read_enabled`、`chat_memory_local_write_enabled`、`chat_memory_enabled_for_lite_web_or_vm`、`is_force_chat_memory_enabled`（`application_force=`）、`is_force_chat_memory_enabled_for_lite_web_or_vm`
- ABTest：`ABTestShallowMemento`
- UI 展示：`should_display_core_memory_memory_panel`

### 6.3 本地配置状态：**未生效 / 不存在**

- 本机**未生成** `memory-config.json`（`service.rs` 中为配置文件名模板）。
- `dynamicConfig.log` 反查 `core_memory|chat_memory|local_memory|force_chat_memory|shallow_memento` **零命中**。
- ⇒ **配置实际由云侧下发**，本地无法自证当前开关状态。

`service.rs` 错误变体族（用于失败归因）：

```
ReadConfig / InvalidConfig / SerializeConfig / WriteConfig
ReadAsset / WriteAsset
InvalidWorkflowOutput / WorkflowOutputNotObject
ClearMemory / PathNotFound
```

---

## 7. 产物与数据格式

### 7.1 数据结构

```
ChatMemoryItem  { memory_content, updated_at }            // 4 elements
ChatMemoryContext { user_profile, project_memory, recent_topics,
                    session_memory, items }               // 5 elements
ChatMemoryItemType: UserProfile / ProjectMemory / RecentTopic / SessionMemory / Unknown
```

### 7.2 落盘目录

```
~/.trae-cn/memory/
├── user_profile.md
├── projects/<slug>/<YYYYMMDD>/            ← 路径模板 %Y%m%d，按项目 + 按天分桶
│   ├── session_memory_<id>.jsonl
│   └── topics.md
├── .cleanup/
└── .tmp/
```

### 7.3 `session_memory_*.jsonl` 实测格式

```json
{"intent":"将指定目录中文件名的“系统分析师新版”改为SANEW",
 "actions":["对指定目录中的文件进行批量重命名"],
 "outcome":"23个文件全部重命名完成，文件名前缀由“系统分析师新版”改为“SANEW”",
 "learned":[],
 "message_summary_time":"2026-09-26 12:25:11",
 "message_id":"6ab74916f0340ca05e76f713",
 "compact_summary_meta":{
   "trigger":"auto",
   "mode":"async",
   "server_history_id":"6ab74916f0340ca05e76f713",
   "summary_digest":"48ea80f2fb0a8a15c5b18e900a29ab370b7d6c85bbe31bb093e75fe962a5e220",
   "created_at_ms":1790396711780}}
```

**关键判读：**

- `compact_summary_meta.trigger = "auto"` + `mode = "async"`
  ⇒ **写入由自动上下文压缩（auto compact）驱动、异步落盘**。
- 文件名中的 hash 与行内 `message_id` **差 1**（如文件 `...f712` vs 首行 `...f713`），属正常现象。
- 服务端另有 `[fetch_message_overview] summary appended to jsonl, session_id=, path=` / `[fetch_message_overview] empty chat_session_ide`。

### 7.4 `topics.md` 格式

走**另一条 topics 抽取流程**（非 compact 驱动），带抬头：

```
[session_id: 6aae7e56df9ce36aca0df5ac | topic_summary_time: 2026-09-19 21:40:10]
<摘要正文>
```

### 7.5 容量限制（UI 文案，`out/nls.zh-cn.messages.json`）

> 「记忆文件」/「此文件自动保存 TRAE 关于当前项目的记忆，仅在本地生效。」/「请不要修改文件名/文件路径。」/「如果你认为某条记忆不符合预期可以手动修改，默认前 … 字符记忆生效。」/「超出{0}字符记忆不生效」

⇒ 存在**字符上限**，超出部分静默不生效。

---

## 8. 本机实测证据

### 8.1 全局目录（2026-09-26 观测）

```
~/.trae-cn/memory/
├── user_profile.md        761 B,  2026-09-19 07:04
├── projects/              21 项 / 19 个项目, 2026-09-21 23:10
├── .cleanup/              2026-09-26 12:25
└── .tmp/                  2026-09-25 19:59
```

### 8.2 本项目（对照组：零写入）

项目 slug：`-home-lucky-caj-viewer--p2-dc5384edc8c60f5b6925`

```
20260913/session_memory_6aa5ec4ff487b3f328547242.jsonl   4109 B, 09-13 09:16
20260913/topics.md                                        1477 B, 09-13 15:46
20260913/session_memory_6aa60463f487b3f3285472ce.jsonl   7687 B, 09-13 15:50
20260913/session_memory_6aa69cf4f4af660381c7b125.jsonl   5436 B, 09-13 21:39
20260919/topics.md                                        1510 B, 09-19 21:43
20260919/session_memory_6aae7e56df9ce36aca0df5ac.jsonl   7642 B, 09-19 22:24
```

- **无 `20260926/` 目录**；最后写入仍是 **09-19**。
- 当日 `renderer.log`：`compact` 计数 = **0**，无 `compact_finish`。
- ⇒ **未触发 auto compact ⇒ 当日零写入**。

### 8.3 对照组（当日有写入）

项目 slug：`-run-media-lucky-Learing------------p2-96642a859ae3fce1dcfc`

```
20260926/session_memory_6ab74916f0340ca05e76f712.jsonl   1139 B, 09-26 12:27（2 行，无 topics.md）
```

- 当日发生**两次 auto compact** ⇒ 连写 **2 条**（12:25:11 / 12:27:22）。
- 两条均为「批量重命名文件」类短任务，收敛后触发压缩并落盘。

### 8.4 日志侧旁证

`~/.config/Trae CN/logs/20260926T184355/window2/renderer.log`（8.2 MB）中 memory 相关命中仅两类：

1. 用户查询原文含「记忆」字样；
2. `checkFilePathExist` 只**读**本项目 `20260919/session_memory_6aae7e56….jsonl` 与对照组 `20260926/session_memory_6ab74916….jsonl`（均 `exist:true`）。

```bash
# 复现命令
grep -c "compact" ~/.config/Trae\ CN/logs/20260926T184355/window2/renderer.log
grep -i "core_memory\|chat_memory\|local_memory\|force_chat_memory\|shallow_memento" \
     ~/.config/Trae\ CN/logs/20260926T184355/*/dynamicConfig.log     # 零命中
```

---

## 9. 关键结论

1. **写入链条的触发权在云侧。** 本地无任何可编程接口能"命令它写一条记忆"。
2. **实际驱动源是 auto compact。** 任务不够长、上下文没被压缩过，就不会有记忆 —— 这让"写完功能就有记忆"的期望**不可靠**。
3. **开关状态本地不可见。** `memory-config.json` 不存在，配置日志零命中 ⇒ 无法自证 `enabled` / `force_enabled` 当前为何。
4. **产物有字符上限**，超出部分静默丢弃。

### 对本项目的决策影响

> **Trae 自带记忆不能作为可靠的 L2（周级语义记忆）载体。**

理由：L2 的准入标准是「一条命令可自证」。该机制违背：

- 写入时机**由云侧 auto compact 决定** → 本地不可控；
- 开关状态**由云侧配置决定** → 本地不可见；
- 无本地 API/SDK 可显式触发或校验 → **不可自证**。

**L2 若要做，需另选载体**（例如自管的、进 git 可 diff 的语义记忆文件）。

---

## 10. 未钉死项与后续验证方案

### 10.1 未钉死项

| 项 | 状态 | 原因 |
|---|---|---|
| `chat_memory_scene` 具体取值清单 | **未取得** | 由云侧随事件下发；`libharness.so` / `libai_agent.so` 字符串池均无变体字面量；`dynamicConfig.log` 无配置命中 |

### 10.2 后续验证方案（若需继续深挖）

**运行时观测法**（比继续静态考古信息量高）：

1. 在本项目内制造一段**足够长的对话**，直至触发 auto compact；
2. 观测 `~/.trae-cn/memory/projects/-home-lucky-caj-viewer--p2-dc5384edc8c60f5b6925/` 是否新增 `20260926/`；
3. 若新增，检查是否有 `topics.md`（区分 compact 驱动与 topics 流程）；
4. 同时抓取该时段的 renderer.log，确认 `cloud SSE chat_memory_trigger received` 与 `scene=` 取值。

> 收益递减提示：即使拿到 scene 取值，也改变不了 §9 的结论（触发权与开关仍在云侧）。

---

## 附录 A：字节偏移取证清单

### A.1 `libharness.so`（196,922,368 B）

`/usr/share/trae-cn/resources/app/modules/ai-agent/libharness.so`

| 偏移 | 内容 |
|---|---|
| 36424800–36426600 | `harness/core/src/stream_ext/handlers/memory_trigger.rs:59`（模块 `harness_core::stream_ext::handlers::memory_trigger`；字段 `scene`；`cloud SSE chat_memory_trigger received` / `failed to fire memory trigger hook`）**← 本轮决定性证据** |
| 20519040 / 37805755 | `chat_memory_scene` |
| 20519232 / 37805772 | `force_update` |
| 20523916 | `builtin:memory_runtime` |
| ~20519000–20523000 | `llm_extractor.rs` 输入字段 + 两条 skip 文案 |
| ~20540000–20541000 | `ChatMemoryItemType` 变体 `UserProfile ProjectMemory RecentTopic SessionMemory Unknown` |
| ~36333500 | 事件名枚举（含 `compact` / `compact_finish` / `chat_memory_trigger`） |
| 16619771 / 16672481 / 17382999 / 17393902 | 配置键族 `chat_memory local_memory chat_memory_with_history past_chats code_as_tool` |
| 16531277 / 16931471 / 17332107 / 17350732 | `chat_memory_user_config` / `disable_exit_plan_mode_tool` / `visible_session_ids` / `prev_turn_user_canceled` |

### A.2 `libai_agent.so`（259 MB）

`/usr/share/trae-cn/resources/app/modules/ai-agent/libai_agent.so`

| 偏移 | 内容 |
|---|---|
| 10961897 | `chat_memory_trigger_event`（日志 `handle_chat_memory_trigger_event: scene=, force_update=`） |
| 11384528 | `chat_memory_scene`（日志 `Unknown chat_memory_scene:`；紧邻 `.../src/handler/chat/handler.rs`） |
| 12233403 | `[fetch_topics_memory] invoked: invocation_id=, session_id=, force_update=, history_ids_count=` / `[fetch_topics_memory] empty history_ids, session_id=` |
| 12520640 | `extract_topics_and_update_memory` |
| 15402274 | `TopicsMemoryPayload` / `TopicRenderVariables` / `force_update` / `SummaryMessageExtraContext` / `SummaryMessageRenderVariables` |
| 15756813 | `ChatMemoryTriggerEvent` serde |
| ~15698300 | SSE 事件名枚举（含 `chat_memory_trigger`） |
| 15699756 | `<statistics>core memory done: add=, evicted=, update=, delete=, failed=</statistics>` |
| 15321400 起 | `domain/plan/memory.rs` 行号簇：269 / 710 / 724 / 764 / 767 / 770 / 986 / 994 / 1009 / 1020 / 1123 / 1133 / 1141 / 1191 / 1201 / 1211 / 1225 / 1252 / 1259 / 1288 / 1297 / 1305 / 1311 / 1319 / 1327 / 1341 / 1354 / 1370 / 1387 / 1510 / 1598 / 1622 / 1630 / 1640 |
| 11120219 / 12120350 | `chat_memory_local_read_enabled` |
| 11759977 / 11864158 | `chat_memory_local_write_enabled` |
| 14814606 | `chat_memory_config` / `local_memory_config` / `chat_memory_with_history` / `chat_skill_recommend` |

### A.3 复现命令模板

```bash
SO=/usr/share/trae-cn/resources/app/modules/ai-agent/libharness.so
OFF=$(grep -abo "chat_memory_scene" "$SO" | head -1 | cut -d: -f1)
dd if="$SO" bs=1 skip=$((OFF-400)) count=1200 2>/dev/null | strings
```

---

## 附录 B：与本项目的记忆分层的关系

| 层 | 载体 | 状态 |
|---|---|---|
| L1 会话交接（日级 episodic） | `docs/handoff/HANDOFF.md` | **已落地**，单文件滚动更新 |
| L2 自动记忆（周级 semantic） | 原拟用 Trae 自带 → **本报告否决** | 未启动，需另选可自证载体 |
| L3 项目知识库（月级 procedural） | 未定 | 未授权 |

四条铁律（L1 已验证遵守）：写成像代码而非文档；稳定知识与易变状态分流；**一条命令可自证**；进 git 可 diff；根文件 30–60 行；**过期记忆比没有记忆更有害**。
