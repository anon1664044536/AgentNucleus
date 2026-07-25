# AgentNucleus Top Runtime

`agent_runtime_top` 是 AgentNucleus 的模型与工具执行层，目标平台为
openKylin/openEuler Linux。Windows 仅作为代码编辑环境；构建、运行和
Unix IPC 验证必须在 openKylin、Linux 虚拟机或 WSL2 中完成。

详细的不变量、序列布局和失败回滚机制见
[`docs/architecture.md`](docs/architecture.md)。

它与系统底座 `agent-runtime` 的职责边界如下：

| 层 | 主要职责 |
|---|---|
| `agent-runtime` | Agent 生命周期、依赖图、动态生成、资源准入、超时/取消、cgroup、进程执行、结果生命周期 |
| `agent_runtime_top` | 真实 LLM 推理、上下文租约、KV Cache 复用/量化/隔离、工具注册和执行 |
| 本地协议 | `SOCK_SEQPACKET` 控制消息，`SCM_RIGHTS + memfd` 传输依赖和结果 |

```text
agentctl
   |
agentd / AgentScheduler
   |  InvocationRequest + dependency memfd
   v
agent_topd
   +-- ContextManager ---- Agent context_id -> private llama seq_id
   +-- llama.cpp adapter - inference / prefix KV / KV type / cancellation
   `-- ToolManager ------- echo / concat / optional shell
   |
   `---- InvocationResponse + output memfd ----> agentd result store
```

## 已实现能力

- 真实 `llm_inference()`：调用当前仓库中的 llama.cpp C API，支持 CPU
  推理、分块 prompt decode、采样、增量生成、超时/断连取消和失败回滚。
- 上下文隔离：每个上下文租约分配独立的 llama `seq_id`；`seq_id=0`
  不用于 Agent，避免不同 Agent 的多轮 KV 相互污染。
- 安全的上下文委托：子 Agent 只有在底层 Runtime 提供有效
  `parent_id + context_id` 时才能加入上下文；释放使用所有者引用计数。
- 共享系统前缀：每个模型保留 8 个不可变高位序列。相同 system
  prompt 命中后通过 `llama_memory_seq_cp()` 复用 KV 元数据，不重新
  decode 前缀；缓存满后按 LRU 淘汰。
- KV Cache 压缩：通过 llama.cpp 的 `type_k/type_v` 真正以 `f16`、
  `q8_0` 或 `q4_0` 精度创建 KV；不是对普通字节缓冲区做模拟压缩。
- 上下文换页：驻留槽满时，将未被调用占用的最久未使用上下文通过
  llama.cpp sequence state 写入权限为 `0600` 的检查点，按需恢复。
- Chat template：默认读取 GGUF 模型内置模板，维护每个 context 的
  user/assistant 历史，并只 decode 新增的模板片段；不支持模板的模型
  自动退回原始 prompt。
- 统一模型/工具接口：模型和工具使用同一 Invocation 协议、资源请求、
  deadline、取消信号、性能指标和共享内存结果。
- 工具管理器：内置 `echo`、`concat`；`shell` 默认关闭，必须显式启用。
- 可观测指标：返回执行时间、输入/输出 token、复用 token、cache hit
  和序列状态大小；`stats` 操作返回累计数据。

旧的 `agent.c`、`scheduler.c`、`resource.c`、`kv_cache.c` 是早期独立
原型，不在当前 CMake 目标中。生产路径以 `agent-runtime` 的调度/资源
模块和本目录的 `TopRuntime`/llama.cpp 适配器为准。

## 目录

```text
agent_runtime_top/
├── apps/
│   ├── agent_topd.cpp
│   ├── context_swap_bench.cpp
│   └── prefix_bench.cpp
├── include/
│   ├── context_manager.h
│   ├── llm.h
│   ├── tool_manager.h
│   └── top_runtime.h
├── src/
│   ├── context_manager.cpp
│   ├── llm_llamacpp.c
│   ├── tool_manager.cpp
│   └── top_runtime.cpp
├── tests/
├── CMakeLists.txt
└── CMakePresets.json
```

## 1. 在 openKylin/WSL 验证模拟后端

先确认已安装 GCC 12+、CMake 3.20+、Ninja 和 pthread：

```bash
cd /path/to/OpenKylinSystem/agent_runtime_top
cmake --preset openkylin-debug
cmake --build --preset openkylin-debug
ctest --preset openkylin-debug
```

该预设不加载 GGUF，用于验证上下文管理、工具管理、TopRuntime 以及
Unix Socket/memfd 端到端协议。

运行模拟服务：

```bash
./build/debug/agent_topd --alias qwen2
```

## 2. 构建真实 CPU 推理

目录默认约定如下：

```text
OpenKylinSystem/
├── agent-runtime/
├── agent_runtime_top/
└── llama.cpp/
```

构建：

```bash
cd /path/to/OpenKylinSystem/agent_runtime_top
cmake --preset openkylin-llama-cpu
cmake --build --preset openkylin-llama-cpu -j
```

启动一个 CPU 模型服务：

```bash
./build/llama-cpu/agent_topd \
  --model /path/to/model.gguf \
  --alias qwen2 \
  --threads 8 \
  --ctx-tokens 4096 \
  --batch-tokens 512 \
  --max-contexts 16 \
  --max-swapped-contexts 32 \
  --swap-dir "$XDG_RUNTIME_DIR/agentnucleus-contexts" \
  --max-tokens 256 \
  --gpu-layers 0 \
  --kv-type q8_0
```

`q8_0` 可减少 KV 内存，但具体模型/后端若不支持某种 KV 类型，
llama.cpp 会拒绝创建 context；此时改用 `f16`。

`--max-contexts` 是同时驻留在 llama KV 中的硬上限；
`--max-swapped-contexts` 是额外允许的磁盘检查点数量。只设置后者时会
使用安全的临时目录；设置 `--swap-dir` 可以固定目录。若不需要换页，
不要传这两个参数。

## 3. 与系统底座联调

`agent_topd` 和 `agentd` 默认使用同一个用户级 socket：

```text
$XDG_RUNTIME_DIR/agentnucleus-top.sock
```

另开终端启动底座：

```bash
cd /path/to/OpenKylinSystem/agent-runtime
./build/debug/agentd --serve --workers 4
```

首次模型调用：

```bash
./build/debug/agentctl invoke 300 warm-prefix \
  model qwen2 warmup_prefix --timeout-ms 60000 -- \
  '{"system_prompt":"You are a power query assistant."}'

./build/debug/agentctl invoke 301 first-turn \
  model qwen2 generate \
  --cpu 8 --memory-mib 512 --timeout-ms 60000 -- \
  '{"system_prompt":"You are a power query assistant.","prompt":"解析用户需求"}'

./build/debug/agentctl wait 301 65000
./build/debug/agentctl status 301
./build/debug/agentctl result 301
```

`status`/`list` 会显示返回的 context ID。使用该 ID 让子 Agent 继续：

```bash
./build/debug/agentctl spawn-invoke 301 302 follow-up \
  model qwen2 generate \
  --context CONTEXT_ID --timeout-ms 60000 -- \
  '{"prompt":"基于上一步继续生成查询语句"}'
```

调用统一工具接口：

```bash
./build/debug/agentctl invoke 401 echo-tool \
  tool echo execute --timeout-ms 5000 -- 'hello'
```

查询 Top Runtime 指标：

```bash
./build/debug/agentctl invoke 402 top-stats \
  model qwen2 stats --timeout-ms 5000 -- '{}'
```

上下文由 Agent 所有。清理任务应作为所有者的动态子 Agent 提交：

```bash
./build/debug/agentctl spawn-invoke 301 303 release-first \
  model qwen2 release_context \
  --context CONTEXT_ID --timeout-ms 5000 -- '{}'

./build/debug/agentctl spawn-invoke 302 304 release-follow-up \
  model qwen2 release_context \
  --context CONTEXT_ID --timeout-ms 5000 -- '{}'
```

最后一个所有者释放后，私有 `seq_id` 的 KV 会被清空并回收到空闲池。

## 4. 共享前缀基准

真实模型构建会同时生成 `agent_top_prefix_bench`。它用同一个长 system
prompt 发起多个独立 Agent 请求：第 0 轮为冷编码，后续轮次应命中共享
前缀，并输出 CSV 和冷/热延迟比。

```bash
./build/llama-cpu/agent_top_prefix_bench \
  --model /path/to/model.gguf \
  --rounds 10 \
  --threads 8 \
  --kv-type q8_0 | tee prefix-q8.csv
```

基准把输出限制为 1 token，以突出前缀 decode 开销。正式实验建议每种
KV 类型至少重复 5 次，报告中位数，并同时记录 `reused_tokens`、
`cache_bytes`、进程 RSS 和模型/CPU/编译参数。

## 5. 上下文换页基准

`agent_top_context_bench` 将驻留上限设为 1，依次创建上下文 A、创建
上下文 B、恢复并续写 A，因此确定触发两次换出和一次换入。它同时输出
端到端耗时与纯推理耗时，二者的差值包含检查点 I/O 和 Runtime 管理开销。

```bash
./build/llama-cpu/agent_top_context_bench \
  --model /path/to/model.gguf \
  --threads 8 \
  --ctx-tokens 2048 \
  --batch-tokens 256 \
  --max-tokens 4 \
  --swap-dir "$XDG_RUNTIME_DIR" \
  --kv-type f16 | tee context-swap.csv
```

命令成功退出且 `stats` 中至少出现 `"swap_outs":2`、`"swap_ins":1`，
说明真实 llama sequence 检查点路径工作正常。完整的 openKylin 验收
流程见 [`docs/openkylin-validation.md`](docs/openkylin-validation.md)。

## Invocation payload

模型 `generate` 接受 UTF-8 原始文本，或以下 JSON 字段：

```json
{
  "system_prompt": "optional, encoded only on the first turn",
  "prompt": "required",
  "max_tokens": 256,
  "temperature": 0.7,
  "top_p": 0.9
}
```

模型控制操作还包括：

- `warmup_prefix`：提前编码 `system_prompt`；
- `stats`：读取累计推理与上下文指标；
- `release_context`：释放调用 Agent 或其受信任父 Agent 的上下文引用。

底层 Agent 的依赖结果不会复制进协议包，而是作为只读 memfd 映射。
Top Runtime 会把 `text_utf8/json_utf8` 依赖追加到模型 prompt；工具则
直接接收映射引用。

## 当前边界

- 当前一个模型对应一个 llama context；为保证 llama context 线程安全，
  同一模型的推理请求串行化。当前的 `--batch-tokens` 是单请求 prompt
  分块大小，不是跨 Agent 连续批处理；后者需要异步请求队列和逐 token
  公平调度，不能通过放宽互斥锁安全实现。
- 共享前缀采用精确字符串匹配，最多 8 个条目，使用 LRU 淘汰；尚未
  实现 token 级最长公共前缀匹配。
- 换页检查点只保证在同一个 `agent_topd` 进程、同一模型和同一
  llama.cpp 构建内恢复，不是跨版本持久化会话格式。
- 自动 chat template 依赖 GGUF 内置模板；可通过
  `--no-chat-template` 回退为原始 prompt。
- 当前每个 `agent_topd` 实例加载一个模型；多模型路由可通过多进程和
  不同 socket 实现，进程内多模型路由尚未开放。
