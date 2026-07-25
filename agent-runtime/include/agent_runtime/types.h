#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agent_runtime {

using AgentId = std::uint64_t;
using ContextId = std::uint64_t;

constexpr AgentId kInvalidAgentId = 0;
constexpr ContextId kInvalidContextId = 0;

enum class AgentState : std::uint8_t {
    created,
    waiting_dependency,
    ready,
    waiting_resource,
    dispatched,
    running,
    blocked,
    suspended,
    completed,
    failed,
    cancelled,
};

const char *to_string(AgentState state) noexcept;
bool is_terminal(AgentState state) noexcept;

enum class InvocationKind : std::uint8_t {
    model = 1,
    tool = 2,
};

const char *to_string(InvocationKind kind) noexcept;

struct ResourceRequest {
    std::uint32_t cpu_threads{1};
    std::uint64_t memory_bytes{0};
    std::chrono::milliseconds timeout{0};
};

struct InvocationSpec {
    InvocationKind kind{InvocationKind::model};
    std::string target;
    std::string operation;
    std::string payload;
    ContextId context_id{kInvalidContextId};
};

struct AgentPerformanceMetrics {
    std::uint64_t queue_time_us{0};
    std::uint64_t execution_time_us{0};
    std::uint64_t input_tokens{0};
    std::uint64_t output_tokens{0};
    std::uint64_t reused_tokens{0};
    std::uint64_t cache_bytes{0};
    bool cache_hit{false};
};

struct AgentTaskSpec {
    AgentId id{kInvalidAgentId};
    AgentId parent_id{kInvalidAgentId};
    std::string name;
    std::string kind;
    int priority{0};
    std::vector<AgentId> dependencies;
    ResourceRequest resources;
    std::uint32_t retry_limit{0};
    std::vector<std::string> command;
    std::optional<InvocationSpec> invocation;
};

struct AgentSnapshot {
    AgentTaskSpec spec;
    AgentState state{AgentState::created};
    std::uint32_t unresolved_dependencies{0};
    std::uint32_t retry_count{0};
    ContextId context_id{kInvalidContextId};
    std::int64_t process_id{-1};
    std::string error;
    AgentPerformanceMetrics metrics;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point finished_at;
};

}  // namespace agent_runtime
