# PMS MQL-to-SQL multi-Agent demo

This demo connects the system substrate and the inference layer in one
openKylin workflow:

1. Agent A converts the user question into database-independent MQL.
2. Agent B reads the same question and `pms_schema.sql`, then selects a minimal
   sub-schema.
3. Agent C waits for A and B and converts their two outputs into MySQL.

The workflow is a DAG, not a client-side sequence. A and B are independently
dispatchable. C receives their results through sealed `memfd` descriptors and
read-only mappings. The identical system prompt also lets the Top Runtime
demonstrate shared-prefix KV reuse. If only one llama model execution lane is
configured, A and B are logically concurrent at the scheduler while decode is
serialized by that model lane.

## Build

From the monorepo root on openKylin:

```bash
(cd agent-runtime && cmake --preset openkylin-debug &&
 cmake --build --preset openkylin-debug)
(cd agent_runtime_top && cmake --preset openkylin-llama-cpu &&
 cmake --build --preset openkylin-llama-cpu)
```

## Start both daemons

Find the GGUF model already stored under `llama.cpp/models`:

```bash
MODEL="$(find "$PWD/llama.cpp/models" -type f -name '*.gguf' -print -quit)"
test -n "$MODEL"
```

Terminal 1:

```bash
./agent_runtime_top/build/llama-cpu/agent_topd \
  --model "$MODEL" \
  --alias pms \
  --ctx-tokens 32768 \
  --batch-tokens 512 \
  --threads 8 \
  --max-contexts 2 \
  --max-swapped-contexts 4 \
  --swap-dir "${XDG_RUNTIME_DIR:-/tmp}/agentnucleus-contexts" \
  --max-tokens 1024 \
  --gpu-layers 0 \
  --kv-type q8_0
```

Use `--kv-type f16` if the selected llama.cpp build or model rejects `q8_0`.
The 32K context is intentional: the supplied schema is about 49 KiB and does
not fit the small 2K/4K settings used by smoke benchmarks.

Terminal 2:

```bash
./agent-runtime/build/debug/agentd --serve --workers 3
```

Both daemons use the current user's default Unix sockets. They must run as the
same user.

## Run

Terminal 3:

```bash
./agent-runtime/build/debug/agent_mql_sql_demo \
  --schema ./pms_schema.sql \
  --alias pms \
  --question '查询2025年投运且电压等级为110kV的主变压器，按运维单位统计数量。'
```

The program prints the DAG, all lifecycle transitions, the three model outputs,
and per-Agent `context`, `execution_time_us`, token count, prefix-cache hit and
reused-token metrics. It uses time-derived Agent IDs so the demo can be run
repeatedly against a persistent `agentd`. Pass `--id-base N` when deterministic
IDs are needed.

Input validation without either daemon is available through:

```bash
./agent-runtime/build/debug/agent_mql_sql_demo \
  --schema ./pms_schema.sql --dry-run
```

Use `--no-cleanup` only when inspecting retained KV contexts manually. Normal
runs submit authorized child cleanup invocations and release all shared result
handles.

## Expected scheduling behavior

- A and B move to `READY/DISPATCHED/RUNNING` without waiting for each other.
- C remains `WAITING_DEPENDENCY` until both complete.
- If A or B fails, dependency-failure propagation cancels C instead of feeding
  it a partial result.
- C's input packet contains only metadata and file descriptors; the potentially
  large intermediate JSON bodies remain in shared memory.
