# Agent Runtime System

这是基于大语言模型（如 LLaMA / Qwen）的 Agent 运行时系统，旨在提供高性能、低显存占用的多 Agent 并发执行环境，并支持硬件级别的上下文调度。

目前整个项目采用 Monorepo（单一代码库）架构进行组织。

## 目录结构

本仓库包含以下核心模块：

### 1. [agent-runtime](./agent-runtime) (基础运行时)
底层的 Agent 基础运行框架。负责：
- 基础的 Agent 注册与生命周期管理
- 底层事件循环机制
- 基础工具和接口的定义

### 2. [agent_runtime_top](./agent_runtime_top) (顶层运行时环境)
Agent 系统的顶层调度与高级内存管理服务 (Daemon)。基于 `llama.cpp` 构建，负责连接底层 Runtime 并实现复杂的 AI 推理和上下文调度策略。主要功能包括：
- **模型推理与取消机制**：真实的 LLaMA / Qwen 推理引擎，支持请求的中断和回滚。
- **Agent 上下文租约与隔离**：支持私有 `seq_id` 隔离，以及父子 Agent 的任务委托。
- **高级系统提示词共享**：支持共享 System Prompt KV Cache，内置 8 槽 LRU 淘汰与访问刷新机制，大幅降低显存开销。
- **上下文换入换出 (Swap)**：支持上下文“驻留”和“磁盘换出”状态的调度；支持 llama sequence 的 checkpoint/restore。
- **灵活的工具调用**：支持统一模型/工具调用（内置 echo、concat、可选 shell 等）。
- **多轮 Chat Template 修复**：精确追踪 KV 位置，支持模板尾部换行及容错机制。

## 开发与构建环境

当前主要在 **openKylin** (或 WSL2) 环境下进行 C++ 开发与测试，使用 `CMake` 进行构建。代码已通过 C++20 语法检查，并带有完整的 Presets 支持。

### 快速构建示例 (以 agent_runtime_top 为例)

```bash
cd agent_runtime_top

# Debug 构建
cmake --preset openkylin-debug
cmake --build --preset openkylin-debug -j
ctest --preset openkylin-debug

# 真实 CPU 推理构建
cmake --preset openkylin-llama-cpu
cmake --build --preset openkylin-llama-cpu -j
```

### 运行端到端测试与基准

```bash
# 启动 Context Swap 磁盘换页基准测试
./agent_runtime_top/build/llama-cpu/agent_top_context_bench \
  --model /absolute/path/model.gguf \
  --threads 8 \
  --ctx-tokens 2048 \
  --batch-tokens 256 \
  --max-tokens 4 \
  --swap-dir "$XDG_RUNTIME_DIR" \
  --kv-type f16
```

## 注意事项

- 本工程依赖外部的 [llama.cpp](https://github.com/ggerganov/llama.cpp) 作为推理后端（已在 `.gitignore` 中忽略，需独立克隆或作为依赖管理）。
- `agent_runtime_top` 需要最新的 llama.cpp C API 支持。

---
*Developed on openKylin System.*
