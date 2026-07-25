# Top Runtime architecture and invariants

## Boundary with `agent-runtime`

The lower Runtime is the source of truth for task identity, parentage,
dependencies, scheduling, resource admission, deadlines and cancellation.
The Top Runtime is a separate same-user process and does not embed another
Agent scheduler.

One invocation contains:

- `agent_id`, trusted `parent_id`, priority and resource request;
- model/tool target, operation and payload;
- an optional opaque `context_id`;
- zero or more immutable dependency buffers transferred as file descriptors.

The response contains an optional output memfd, context ID, backend PID,
status and performance metrics. The lower Runtime owns the returned result
lifetime after validating and sealing that memfd.

## llama sequence layout

For `max_contexts = N`, `agent_topd` creates the llama context with:

```text
n_seq_max = N + 8 + 1
```

The sequence namespace is partitioned as follows:

```text
0                  reserved, never assigned to an Agent
1 .. N             private mutable Agent contexts
N+1 .. N+8         immutable shared system-prefix entries
```

The C adapter rejects a private request whose sequence ID falls outside
`[1, N]`. `llm_clear_seq()` applies the same validation, so a normal context
release cannot delete a shared prefix.

Each exact system-prompt string is encoded once into one high sequence.
`llama_memory_seq_cp()` then associates the prefix positions with an Agent's
private sequence. With unified KV memory this reuses the underlying KV cells
and only changes sequence membership metadata. All eight prefix slots carry
an access clock; a miss on a full cache removes the least-recently-used
reserved sequence before encoding the new prefix.

## Context lease rules

`ContextManager` exposes opaque monotonically increasing context IDs; callers
never select llama sequence IDs directly.

1. `context_id = 0` creates or reuses the calling Agent's default context.
2. An existing context can be acquired by an existing owner.
3. A child can acquire it when the lower Runtime supplies a parent ID that is
   already an owner.
4. Every acquiring Agent becomes an owner and must release its reference.
5. A dynamically spawned cleanup Agent may release its trusted parent's
   reference.
6. The final release clears the private llama sequence and returns that slot
   to the free list.

An acquire pins its context until inference finishes. A pinned context cannot
be selected for swap or be finally released.

When disk swap is enabled, logical context capacity can exceed resident
sequence capacity. If no private sequence is free, the manager selects the
least-recently-used unpinned resident context, writes its llama sequence state
to a mode-0600 file, clears the sequence, and marks the logical context as
swapped. Accessing that context evicts another candidate if necessary and
restores the checkpoint into the obtained private sequence.

Explicit context sharing is mutable continuation, not an immutable branch:
calls using the same context are serialized and extend the same conversation.
Independent Agent reasoning must use different context IDs; common immutable
knowledge should use the shared system-prefix cache instead.

## Failure and cancellation

One mutex serializes operations on a llama context because llama decode and
KV mutation are not safe to run concurrently on the same context. Tool calls
remain concurrent.

Before a turn, the adapter records the sequence's original position. Prompt
decode and token generation periodically poll the Invocation connection.
The same callback is installed as llama.cpp's CPU abort callback. On timeout,
client disconnect or decode failure, all positions added by that turn are
removed and the previous sequence state is restored.

Failures in one invocation are returned as `failed` or `cancelled`; they do
not terminate `agent_topd` and do not clear other Agents' sequences.

## Chat formatting

For models with a GGUF chat template, the Top Runtime stores role/content
history per logical context. On the first turn it renders the system-only
prefix separately, allowing that formatted prefix to use the shared KV cache.
It then decodes only the remaining user and assistant-generation markers.

After a response, the role/content history is retained. The next turn renders
the previous history and the extended history, verifies that the former is an
exact prefix, and submits only the suffix. A sampled or forced end-of-turn
token is persisted in the private sequence but omitted from visible output,
keeping the KV position aligned with the rendered role history. This follows
llama.cpp's incremental-chat behavior, including its trailing-newline rule.
Conversation history remains in host memory while its llama sequence is
swapped to disk.

## Concurrency boundary

`n_batch` controls chunking within one prompt. It is not cross-Agent
continuous batching. The current adapter serializes mutation of a model's
single llama context; context pins, cancellation rollback, checkpointing, and
prefix eviction all rely on that invariant.

A future continuous-batching engine must replace the synchronous
`llm_inference()` loop with a request queue that owns one mixed
`llama_batch`, records a row-to-sequence mapping, samples each active sequence
independently, and applies deadline/fairness policy between decode steps.
Simply removing the mutex would race the shared llama context and corrupt KV
state.

## Resource mapping

- `ResourceRequest.cpu_threads` selects the number of llama decode threads,
  capped by `agent_topd --threads`.
- Deadline/cancellation is enforced over the Invocation connection.
- Context count is a hard admission boundary independent of host RAM.
- K/V precision is selected when the llama context is allocated.
- Model and dependency/output memory admission remains owned by the lower
  Runtime's resource ledger.
- The optional shell tool is disabled by default. It is intended only for a
  trusted same-user deployment and should be combined with lower-level
  cgroup policy in production.

## Metrics

| Field | Meaning |
|---|---|
| `execution_time_us` | Top handler inference/tool duration |
| `input_tokens` | Tokens decoded for the current user prompt |
| `output_tokens` | Tokens generated in the current turn |
| `cache_hit` | This turn reused an already encoded system prefix |
| `reused_tokens` | System-prefix tokens skipped by this turn |
| `cache_bytes` | Exact llama sequence state size reported by llama.cpp |

The `stats` operation additionally returns cumulative requests, generated
tokens, latency, token throughput, prefix hits, saved tokens and active
context/owner counts, prefix evictions, swap operations and checkpoint bytes.
