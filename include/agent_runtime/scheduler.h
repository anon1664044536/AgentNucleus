#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agent_runtime/resource_monitor.h"
#include "agent_runtime/types.h"

namespace agent_runtime {

class AgentScheduler {
public:
    bool submit(const AgentTaskSpec &task, std::string *error = nullptr);
    bool submit_batch(const std::vector<AgentTaskSpec> &tasks,
                      std::string *error = nullptr);
    bool spawn(AgentId parent,
               AgentTaskSpec task,
               std::string *error = nullptr);

    std::optional<AgentId> try_dispatch(const MemorySnapshot &resources);
    std::optional<AgentId> wait_dispatch(const MemorySnapshot &resources);

    bool mark_running(AgentId id,
                      std::int64_t process_id = -1,
                      std::string *error = nullptr);
    bool mark_blocked(AgentId id, std::string *error = nullptr);
    bool wake(AgentId id, std::string *error = nullptr);
    bool complete(AgentId id, std::string *error = nullptr);
    bool fail(AgentId id,
              std::string message,
              std::string *error = nullptr);
    bool cancel(AgentId id, std::string *error = nullptr);
    bool bind_context(AgentId id,
                      ContextId context_id,
                      std::string *error = nullptr);

    std::optional<AgentSnapshot> snapshot(AgentId id) const;
    std::vector<AgentSnapshot> snapshots() const;
    std::size_t size() const;
    bool all_terminal() const;
    void notify();

private:
    struct Node {
        AgentSnapshot snapshot;
        std::vector<AgentId> successors;
        bool resources_reserved{false};
    };

    bool submit_batch_locked(const std::vector<AgentTaskSpec> &tasks,
                             std::string *error);
    bool graph_has_cycle_locked() const;
    std::optional<AgentId> dispatch_locked(const MemorySnapshot &resources);
    bool transition_locked(Node &node,
                           AgentState next,
                           std::string *error);
    void enqueue_ready_locked(Node &node);
    void release_resources_locked(Node &node);
    void cancel_descendants_locked(AgentId id, const std::string &reason);

    mutable std::mutex mutex_;
    std::condition_variable ready_cv_;
    std::unordered_map<AgentId, Node> nodes_;
    std::unordered_set<AgentId> ready_;
    std::uint64_t reserved_cpu_threads_{0};
    std::uint64_t reserved_memory_bytes_{0};
};

}  // namespace agent_runtime
