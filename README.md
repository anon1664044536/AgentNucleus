# Agent Runtime

Agent Runtime is a Linux user-space execution substrate for multi-agent
workloads. It maps logical agents to schedulable tasks, Linux processes,
cgroup v2 resource domains, llama.cpp context leases, and shared-memory IPC.

The target platforms are openKylin and openEuler. Windows is only intended to
be used as an editor or remote-development host. Configure and run the project
inside openKylin, a Linux virtual machine, WSL2, or a remote Linux machine.

## Implemented modules

- Agent Control Block and lifecycle state machine
- DAG submission, dependency wake-up, dynamic task spawning, retries, and
  failure propagation
- Priority aging and CPU/memory-aware dispatch with concurrent reservations
- Linux process executor with a pre-exec admission barrier
- pidfd-based process observation with waitpid fallback
- cgroup v2 CPU, memory, and PID limits
- `/proc/meminfo`, cgroup memory, and PSI resource sampling
- `memfd_create` shared regions and `SCM_RIGHTS` descriptor transfer
- `eventfd` notification
- Optional CPU-only llama.cpp model and context pool
- Context cancellation, serialization, release, and restoration

## Directory layout

```text
agent-runtime/
|- CMakeLists.txt
|- include/agent_runtime/
|  |- cgroup_manager.h
|  |- model_engine.h
|  |- process_executor.h
|  |- resource_monitor.h
|  |- runtime.h
|  |- scheduler.h
|  |- shared_memory.h
|  `- types.h
|- src/
|- apps/agentd.cpp
|- examples/
`- tests/
```

## Build on openKylin

Install a C++20 compiler, CMake, and Ninja using the package manager available
on the target image. Then run:

```bash
cd agent-runtime
cmake --preset openkylin-debug
cmake --build --preset openkylin-debug
ctest --preset openkylin-debug
```

The equivalent commands without presets are:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAGENT_RUNTIME_WITH_LLAMA=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the basic demonstrations:

```bash
./build/agentd --memory
./build/agentd --demo
./build/agent_pipeline_demo
./build/agent_ipc_demo
```

## Build with llama.cpp CPU inference

By default, the source tree is expected at `../llama.cpp`. It can be changed
with `AGENT_RUNTIME_LLAMA_SOURCE_DIR`.

```bash
cmake -S . -B build-llama -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAGENT_RUNTIME_WITH_LLAMA=ON \
  -DAGENT_RUNTIME_LLAMA_SOURCE_DIR=../llama.cpp \
  -DGGML_NATIVE=ON \
  -DGGML_CUDA=OFF \
  -DLLAMA_CURL=OFF
cmake --build build-llama --parallel
```

To link an existing Linux build instead of rebuilding llama.cpp:

```bash
cmake -S . -B build-llama -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAGENT_RUNTIME_WITH_LLAMA=ON \
  -DAGENT_RUNTIME_LLAMA_INCLUDE_DIR=/path/to/llama.cpp/include \
  -DAGENT_RUNTIME_LLAMA_LIBRARY=/path/to/libllama.so
cmake --build build-llama --parallel
```

If a static `libllama.a` is used, pass its ggml and system dependencies through
`AGENT_RUNTIME_LLAMA_EXTRA_LIBRARIES`. A shared `libllama.so` is simpler for the
first integration test.

Verify model loading and context admission with:

```bash
./build-llama/agent_model_smoke /path/to/model.gguf
```

`ModelEngine` loads one `llama_model` and leases a bounded number of
`llama_context` objects to agents. Each context uses `n_gpu_layers = 0`, so the
engine remains CPU-only. The configured `context_reservation_bytes` is an
admission estimate and should be calibrated with benchmark measurements for
the selected model, context length, KV types, and batch size. Context snapshots
contain llama execution state; token history, sampler state, Agent metadata, and
tool results must be checkpointed by the upper Runtime layer as well.

## cgroup v2 deployment

The runtime does not silently ignore cgroup permission errors. A deployment
must delegate a writable cgroup v2 subtree to the runtime service. The default
path is:

```text
/sys/fs/cgroup/agent-runtime
```

For production, launch `agentd` from a systemd service or scope with cgroup
delegation and set `RuntimeConfig::cgroup_root` to that delegated subtree.
Do not make the entire `/sys/fs/cgroup` hierarchy world-writable.

After delegation is configured, the process/cgroup demonstration is:

```bash
./build/agentd --demo-cgroup
```

Each command agent is stopped behind a pipe barrier immediately after `fork`.
The parent creates the agent cgroup and attaches the child PID before releasing
the barrier and allowing `execvp` to run.

## Shared-memory contract

Large payloads are represented by `SharedBufferRef`:

```text
region_id + region_size + offset + length + type + flags + version
```

The producer writes into a `memfd` mapping and transfers the descriptor over an
`AF_UNIX/SOCK_SEQPACKET` channel. The receiver validates bounds and descriptor
size, maps the same pages, and reads by offset. Raw virtual addresses are never
sent between processes.

## Current scope

This repository is an executable foundation, not a complete competition
submission. The next layers should add a persistent external control protocol,
context ownership/reference counting across process crashes, execution traces,
context-locality scoring, llama token-generation handlers, and benchmark
drivers for scheduling and IPC comparisons.
