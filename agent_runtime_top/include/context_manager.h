#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agent_runtime_top {

using AgentId = std::uint64_t;
using ContextId = std::uint64_t;

struct ContextLease {
    ContextId context_id{0};
    int sequence_id{-1};
    bool created{false};
    bool restored{false};
};

struct ContextSwapCallbacks {
    std::function<bool(ContextId, int, std::string *)> save;
    std::function<bool(ContextId, int, std::string *)> load;
};

struct ContextManagerStats {
    std::size_t active_contexts{0};
    std::size_t active_owners{0};
    std::size_t resident_contexts{0};
    std::size_t swapped_contexts{0};
    std::uint64_t created_contexts{0};
    std::uint64_t shared_acquisitions{0};
    std::uint64_t released_contexts{0};
    std::uint64_t swap_outs{0};
    std::uint64_t swap_ins{0};
};

class ContextManager {
public:
    explicit ContextManager(std::size_t max_resident_contexts,
                            std::size_t max_logical_contexts = 0);

    std::optional<ContextLease> acquire(AgentId agent_id,
                                        AgentId parent_id,
                                        ContextId requested_context,
                                        std::string *error = nullptr,
                                        const ContextSwapCallbacks *swap =
                                            nullptr);
    bool finish(ContextId context_id, std::string *error = nullptr);
    bool release(AgentId agent_id,
                 AgentId parent_id,
                 ContextId context_id,
                 int *released_sequence,
                 std::string *error = nullptr,
                 bool *context_removed = nullptr);
    bool owns(AgentId agent_id, ContextId context_id) const;
    ContextManagerStats stats() const;

private:
    struct Entry {
        ContextId id{0};
        int sequence_id{-1};
        bool resident{true};
        std::size_t pin_count{0};
        std::unordered_set<AgentId> owners;
        std::chrono::steady_clock::time_point last_access;
    };

    std::optional<ContextId> find_for_agent_locked(AgentId agent_id) const;
    bool authorized_locked(const Entry &entry,
                           AgentId agent_id,
                           AgentId parent_id) const;
    std::optional<int> obtain_sequence_locked(
        ContextId excluded_context,
        const ContextSwapCallbacks *swap,
        std::string *error);
    bool make_resident_locked(Entry *entry,
                              const ContextSwapCallbacks *swap,
                              std::string *error);

    const std::size_t max_resident_contexts_;
    const std::size_t max_logical_contexts_;
    mutable std::mutex mutex_;
    std::unordered_map<ContextId, Entry> contexts_;
    std::vector<int> free_sequences_;
    ContextId next_context_id_{1};
    ContextManagerStats stats_;
};

}  // namespace agent_runtime_top
