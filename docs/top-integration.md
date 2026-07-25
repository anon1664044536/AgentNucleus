# `agent_runtime_top` integration contract

## Ownership boundary

`agent-runtime` is the system substrate:

- owns the Agent Control Block, DAG and lifecycle state;
- performs CPU/memory admission and optional cgroup placement;
- propagates cancellation and deadlines;
- owns dependency results and transfers read-only `memfd` duplicates;
- binds the context ID and performance metrics reported by the backend.

`agent_runtime_top` is the invocation backend:

- owns llama.cpp models, sessions and native KV Cache objects;
- validates that an Agent may access a requested context;
- implements prefix reuse, copy-on-write isolation, compression/offload and
  eviction;
- implements external tools or dispatches them to isolated workers;
- returns output in a writable, unsealed `memfd`;
- reports context and cache/token/latency metrics.

The top layer must not maintain a second authoritative Agent lifecycle or DAG.
It may schedule model batches internally, but `agent-runtime` remains the
source of truth for task state.

## Transport

The transport is a same-host Unix `SOCK_SEQPACKET` socket:

- default endpoint:
  `$XDG_RUNTIME_DIR/agentnucleus-top.sock`;
- fallback endpoint: `/tmp/agentnucleus-top-UID.sock`;
- socket mode: `0600`;
- peer check: `SO_PEERCRED` UID must match;
- maximum metadata packet: 1 MiB;
- maximum attached inputs: 64;
- maximum simultaneous client calls: 64;
- descriptor transport: `SCM_RIGHTS`.

The wire protocol is explicitly versioned by
`kInvocationProtocolVersion`. Multi-byte integers are encoded little-endian;
raw pointers are never sent across processes.

## Request

`InvocationRequest` contains:

| Field | Meaning |
|---|---|
| `agent_id`, `parent_id` | Runtime identity and lineage |
| `kind` | `model` or `tool` |
| `target` | Model alias or tool name |
| `operation` | Such as `generate`, `embed`, `execute`, `release_context` |
| `payload` | Small UTF-8/JSON control payload |
| `context_id` | Existing top-owned context, or zero for a new context |
| `priority` | Runtime scheduling priority |
| `resources` | CPU, memory and deadline budget |
| `inputs` | Metadata for attached dependency-result descriptors |

Each input descriptor is mapped read-only. `producer_id` identifies the Agent
that produced it; `SharedBufferRef` validates the region ID, size, offset,
length, content type, flags and version. Completed dependencies without a
published result are dependency-only edges and are not attached.

## Response

`InvocationResponse` contains:

| Field | Meaning |
|---|---|
| `status` | `ok`, `rejected`, `failed`, `cancelled` or `busy` |
| `process_id` | Linux PID executing the model/tool call |
| `context_id` | Context lease selected or created by the top layer |
| `metrics` | Queue/execution time, token counts, KV bytes and reuse data |
| `output` | Metadata for an optional output descriptor |

The output offset must be zero when returned to `agent-runtime`. The output
descriptor must reference a writable, unsealed `memfd`; the runtime then
shrinks and seals it before publishing an immutable result. `InvocationServer`
unmaps the producer's writable view before transferring descriptor ownership,
so `F_SEAL_WRITE` cannot race another writable mapping. A non-`ok` status enters
the normal Agent retry/failure path.

## Adapter skeleton

The top process can use `InvocationServer` and replace the demo handler with
its llama.cpp/resource-manager implementation:

```cpp
agent_runtime::InvocationServer server(
    [&](const agent_runtime::InvocationRequest &request,
        const std::vector<agent_runtime::InvocationMappedInput> &inputs,
        const std::function<bool()> &cancel_requested) {
        agent_runtime::InvocationHandlerResult result;

        // Validate request.agent_id -> request.context_id ownership.
        // Invoke the model or tool and populate a memfd result.
        // Record real cache/token/latency statistics.
        // Check cancel_requested() between decode/generation steps.

        result.context_id = context_id;
        result.metrics.cache_hit = cache_hit;
        result.metrics.reused_tokens = reused_tokens;
        result.output = std::move(output_region);
        result.output_length = output_length;
        result.output_type =
            agent_runtime::SharedDataType::json_utf8;
        return result;
    });
```

The top CMake project can consume the transport without enabling the bottom
layer's own llama.cpp wrapper:

```cmake
set(AGENT_RUNTIME_WITH_LLAMA OFF CACHE BOOL "" FORCE)
set(AGENT_RUNTIME_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AGENT_RUNTIME_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(../agent-runtime agent-runtime-build)

add_executable(agent_topd src/runtime_bridge.cpp)
target_link_libraries(agent_topd PRIVATE
    agent_runtime::agent_runtime
    agent_top_core)
```

Because `InvocationServer` is a C++ API, a C-based top implementation should
keep its KV/model core in C and use one small C++ bridge translation unit. C
headers included by that bridge must expose their functions inside
`extern "C"` guards.

`InvocationServer` accepts calls concurrently so the top layer can batch model
requests or execute independent tools. The registered handler must therefore
be thread-safe; model backends that require serialized mutation should enqueue
requests into their own batching scheduler instead of holding one global lock
for the complete generation.

The example executable `agent_top_bridge_demo` is a transport stub, not a KV
Cache implementation. It proves that the two independently running layers can
exchange requests and dependency/output memory without copying payload bytes
through the control packets.

## Cancellation and crashes

While a call is pending, `agent-runtime` polls the backend socket in short
intervals. A local cancellation or deadline closes the connection and moves
the Agent through its existing cancellation/failure path. The top layer must
treat peer disconnect as cancellation and reclaim any uncommitted context
lease. Conversely, if the top process crashes or disconnects, the call fails
without crashing `agentd`, and the configured retry policy applies.

Long-lived context ownership belongs to the top layer. It should key leases by
both Agent ID and context ID, maintain reference counts, use copy-on-write for
shared prefixes, and offer explicit operations such as `release_context` for
reclamation. A context ID is opaque to `agent-runtime`.
