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
- Per-Agent process groups for descendant cancellation and timeout cleanup
- pidfd-based process observation with waitpid fallback
- cgroup v2 CPU, memory, and PID limits
- `/proc/meminfo`, cgroup memory, and PSI resource sampling
- `memfd_create` shared regions and `SCM_RIGHTS` descriptor transfer
- `eventfd` notification
- Optional CPU-only llama.cpp model and context pool
- Context cancellation, serialization, release, and restoration
- Persistent `agentd` control service over a versioned Unix Domain Socket
- Same-UID control authorization and `0600` socket permissions
- `agentctl` submission, dynamic spawning, inspection, waiting, cancellation,
  and shutdown commands
- Bounded command `stdout`/`stderr` capture into immutable `memfd` results
- Zero-copy result retrieval through `SharedBufferRef` and `SCM_RIGHTS`

## Directory layout

```text
agent-runtime/
|- CMakeLists.txt
|- include/agent_runtime/
|  |- cgroup_manager.h
|  |- control_channel.h
|  |- control_protocol.h
|  |- model_engine.h
|  |- process_executor.h
|  |- resource_monitor.h
|  |- runtime.h
|  |- scheduler.h
|  |- shared_memory.h
|  `- types.h
|- src/
|- apps/agentd.cpp
|- apps/agentctl.cpp
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
./build/debug/agentd --memory
./build/debug/agentd --demo
./build/debug/agent_pipeline_demo
./build/debug/agent_ipc_demo
```

## Persistent runtime control plane

Start the runtime in one terminal. The default socket is
`$XDG_RUNTIME_DIR/agentnucleus.sock`, or `/tmp/agentnucleus-UID.sock` when
`XDG_RUNTIME_DIR` is unavailable.

```bash
./build/debug/agentd --serve --workers 4
```

Submit a command-backed agent and inspect it from another terminal:

```bash
./build/debug/agentctl ping
./build/debug/agentctl submit 100 parse-request \
  --cpu 1 --memory-mib 128 --timeout-ms 5000 -- \
  /bin/sh -c 'printf "{\"intent\":\"power_query\"}"; sleep 1'
./build/debug/agentctl status 100
./build/debug/agentctl wait 100 10000
./build/debug/agentctl list
./build/debug/agentctl result 100
./build/debug/agentctl release-result 100
```

Dynamic children can be added while the daemon is running. Here agent 101 is
bound to parent 100 and also waits for agent 100 to complete:

```bash
./build/debug/agentctl spawn 100 101 generate-query \
  --depends 100 --timeout-ms 5000 -- /bin/sh -c 'exit 0'
```

Cancellation and graceful daemon shutdown are also exposed through the same
protocol:

```bash
./build/debug/agentctl cancel 101
./build/debug/agentctl shutdown
```

Use `--socket PATH` on both programs for a custom endpoint. The daemon creates
the socket with mode `0600`, accepts only clients with the same effective UID,
limits packets to 1 MiB, and refuses to replace an active socket. This control
plane is intentionally local to the host; it is not a network API.

Command output is retained in a sealed shared-memory region owned by the
runtime. `agentctl result ID` receives a duplicated file descriptor and maps
the same pages read-only, so the result body is not copied into the control
packet. Standard output and standard error are merged in write order. The
default per-Agent limit is 1 MiB and can be changed when starting the daemon:

```bash
./build/debug/agentd --serve --workers 4 --max-output-mib 8
```

When output exceeds the configured limit, the runtime continues draining the
child pipe to avoid deadlock, retains the prefix that fits, and marks the
`SharedBufferRef` as truncated. Before publication, the backing region is
shrunk to the retained result length so small outputs do not permanently hold
the full capture capacity.

In-process tool or model handlers can return an already populated
`SharedMemoryRegion` through `AgentExecutionResult::output`, together with its
length, data type, and flags. The runtime applies the same shrinking, sealing,
ownership, and descriptor-transfer path used for command output.

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

The demo executable also accepts the delegated path through the environment:

```bash
AGENT_RUNTIME_CGROUP_ROOT=/path/to/delegated/cgroup \
  ./build/debug/agentd --demo-cgroup
```

After delegation is configured, the process/cgroup demonstration is:

```bash
./build/debug/agentd --demo-cgroup
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

Completed command results add the immutable and stdout/stderr flags. The
runtime seals the `memfd` against writes, growth, and shrinking before making
the descriptor available to consumers. Result storage is reclaimed
automatically when the runtime exits or when an entry is explicitly erased.

## Current scope

This repository is an executable foundation, not a complete competition
submission. The next layers should let dependent command Agents receive input
descriptors automatically, connect llama token-generation handlers to the same
result store, add context ownership/reference counting across process crashes,
execution traces, context-locality scoring, and benchmark drivers for
scheduling and IPC comparisons.
