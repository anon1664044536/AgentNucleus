# openKylin validation checklist

Run this checklist on openKylin after synchronizing `agent-runtime`,
`agent_runtime_top`, and `llama.cpp` under the same parent directory.
Windows is only the editing host for this project.

## 1. Record the environment

```bash
cat /etc/os-release
uname -a
cmake --version
ninja --version
g++ --version
lscpu | grep -E 'Model name|Socket|Core|Thread|NUMA'
free -h
git -C ../llama.cpp rev-parse HEAD
```

Keep this output with the benchmark results so measurements are reproducible.

## 2. Build and test the simulated backend

```bash
cmake --preset openkylin-debug
cmake --build --preset openkylin-debug -j
ctest --preset openkylin-debug
```

Expected result: all context-manager, tool-manager, TopRuntime, and local
Invocation protocol tests pass. This stage does not prove that GGUF inference
or llama sequence checkpoint files work.

## 3. Build the real CPU backend

```bash
cmake --preset openkylin-llama-cpu
cmake --build --preset openkylin-llama-cpu -j
```

Start with `f16` KV. Test `q8_0` and `q4_0` only after the baseline succeeds,
because support depends on the selected llama.cpp backend and model.

## 4. Verify real prefix reuse

```bash
/usr/bin/time -v \
  ./build/llama-cpu/agent_top_prefix_bench \
  --model /absolute/path/model.gguf \
  --rounds 10 \
  --threads 8 \
  --kv-type f16 \
  > prefix-f16.csv 2> prefix-f16-resource.txt
```

Acceptance checks:

- round 0 may report `cache_hit=0`;
- subsequent rounds report `cache_hit=1`;
- `reused_tokens` is greater than zero;
- the process exits with status 0.

Repeat at least five times per KV type and compare median warm latency and
maximum RSS. Do not compare one isolated run.

## 5. Verify real context swap and restore

```bash
mkdir -p "$XDG_RUNTIME_DIR/agentnucleus-bench"
chmod 700 "$XDG_RUNTIME_DIR/agentnucleus-bench"

/usr/bin/time -v \
  ./build/llama-cpu/agent_top_context_bench \
  --model /absolute/path/model.gguf \
  --threads 8 \
  --ctx-tokens 2048 \
  --batch-tokens 256 \
  --max-tokens 4 \
  --swap-dir "$XDG_RUNTIME_DIR/agentnucleus-bench" \
  --kv-type f16 \
  > context-swap-f16.csv 2> context-swap-f16-resource.txt
```

Acceptance checks:

- the process exits with status 0;
- output contains `"swap_outs":2` and `"swap_ins":1`;
- all checkpoint files are created below the configured private parent;
- the per-process child directory is removed after a clean shutdown.

`swap_in_continue.end_to_end_us - swap_in_continue.inference_us` is an
approximation of restore plus Runtime overhead. Filesystem cache effects mean
the first and later runs should be reported separately.

## 6. Verify daemon integration

Terminal A:

```bash
./build/llama-cpu/agent_topd \
  --model /absolute/path/model.gguf \
  --alias qwen2 \
  --threads 8 \
  --ctx-tokens 4096 \
  --batch-tokens 512 \
  --max-contexts 1 \
  --max-swapped-contexts 2 \
  --swap-dir "$XDG_RUNTIME_DIR/agentnucleus-daemon" \
  --max-tokens 64 \
  --gpu-layers 0 \
  --kv-type f16
```

Terminal B:

```bash
cd ../agent-runtime
./build/debug/agentd --serve --workers 4
```

Terminal C submits two independent contexts and then continues the first one.
Use the `context_id` printed by `status`/`result`:

```bash
./build/debug/agentctl invoke 501 context-a \
  model qwen2 generate --timeout-ms 120000 -- \
  '{"system_prompt":"You are concise.","prompt":"Remember marker A."}'
./build/debug/agentctl wait 501 125000
./build/debug/agentctl status 501

./build/debug/agentctl invoke 502 context-b \
  model qwen2 generate --timeout-ms 120000 -- \
  '{"system_prompt":"You are concise.","prompt":"Remember marker B."}'
./build/debug/agentctl wait 502 125000
./build/debug/agentctl status 502

./build/debug/agentctl spawn-invoke 501 503 context-a-next \
  model qwen2 generate --context CONTEXT_A_ID --timeout-ms 120000 -- \
  '{"prompt":"Which marker did I give you?"}'
./build/debug/agentctl wait 503 125000

./build/debug/agentctl invoke 504 runtime-stats \
  model qwen2 stats --timeout-ms 5000 -- '{}'
./build/debug/agentctl wait 504 10000
./build/debug/agentctl result 504
```

The final stats must show nonzero `swap_outs`, `swap_ins`, and
`snapshot_bytes`. Release every owner reference afterward:

```bash
./build/debug/agentctl spawn-invoke 501 505 release-a-parent \
  model qwen2 release_context --context CONTEXT_A_ID \
  --timeout-ms 5000 -- '{}'

./build/debug/agentctl spawn-invoke 503 506 release-a-child \
  model qwen2 release_context --context CONTEXT_A_ID \
  --timeout-ms 5000 -- '{}'

./build/debug/agentctl spawn-invoke 502 507 release-b \
  model qwen2 release_context --context CONTEXT_B_ID \
  --timeout-ms 5000 -- '{}'
```

After all three cleanup tasks complete, another `stats` request should report
`"active_contexts":0`, `"snapshot_count":0`, and `"snapshot_bytes":0`.

## 7. Results to preserve

For the competition report, preserve:

- exact OS, kernel, CPU, RAM, compiler, CMake, llama.cpp commit, and GGUF;
- build type and all Runtime flags;
- raw CSV and `/usr/bin/time -v` output;
- at least five repetitions and median/P95 rather than only the best run;
- a baseline with prefix reuse/swap disabled and the optimized configuration.
