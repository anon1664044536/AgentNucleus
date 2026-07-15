#include "agent_runtime/scheduler.h"

#include <algorithm>
#include <functional>
#include <limits>

namespace agent_runtime {

const char *to_string(AgentState state) noexcept {
    switch (state) {
        case AgentState::created: return "CREATED";
        case AgentState::waiting_dependency: return "WAITING_DEPENDENCY";
        case AgentState::ready: return "READY";
        case AgentState::waiting_resource: return "WAITING_RESOURCE";
        case AgentState::dispatched: return "DISPATCHED";
        case AgentState::running: return "RUNNING";
        case AgentState::blocked: return "BLOCKED";
        case AgentState::suspended: return "SUSPENDED";
        case AgentState::completed: return "COMPLETED";
        case AgentState::failed: return "FAILED";
        case AgentState::cancelled: return "CANCELLED";
    }
    return "UNKNOWN";
}

bool is_terminal(AgentState state) noexcept {
    return state == AgentState::completed || state == AgentState::failed ||
           state == AgentState::cancelled;
}

bool AgentScheduler::submit(const AgentTaskSpec &task, std::string *error) {
    return submit_batch({task}, error);
}

bool AgentScheduler::submit_batch(const std::vector<AgentTaskSpec> &tasks,
                                  std::string *error) {
    std::lock_guard lock(mutex_);
    const bool submitted = submit_batch_locked(tasks, error);
    if (submitted) {
        ready_cv_.notify_all();
    }
    return submitted;
}

bool AgentScheduler::spawn(AgentId parent,
                           AgentTaskSpec task,
                           std::string *error) {
    std::lock_guard lock(mutex_);
    if (!nodes_.contains(parent)) {
        if (error != nullptr) {
            *error = "parent agent does not exist";
        }
        return false;
    }
    task.parent_id = parent;
    const bool submitted = submit_batch_locked({task}, error);
    if (submitted) {
        ready_cv_.notify_all();
    }
    return submitted;
}

bool AgentScheduler::submit_batch_locked(const std::vector<AgentTaskSpec> &tasks,
                                         std::string *error) {
    if (tasks.empty()) {
        if (error != nullptr) {
            *error = "task batch is empty";
        }
        return false;
    }

    std::unordered_set<AgentId> incoming;
    for (const auto &task : tasks) {
        if (task.id == kInvalidAgentId) {
            if (error != nullptr) {
                *error = "agent id 0 is reserved";
            }
            return false;
        }
        if (nodes_.contains(task.id) || !incoming.insert(task.id).second) {
            if (error != nullptr) {
                *error = "duplicate agent id: " + std::to_string(task.id);
            }
            return false;
        }
    }

    for (const auto &task : tasks) {
        for (const AgentId dependency : task.dependencies) {
            if (dependency == task.id ||
                (!nodes_.contains(dependency) && !incoming.contains(dependency))) {
                if (error != nullptr) {
                    *error = "invalid dependency " + std::to_string(dependency) +
                             " for agent " + std::to_string(task.id);
                }
                return false;
            }
        }
    }

    const auto nodes_backup = nodes_;
    const auto ready_backup = ready_;

    const auto now = std::chrono::steady_clock::now();
    for (const auto &task : tasks) {
        Node node;
        node.snapshot.spec = task;
        node.snapshot.state = AgentState::created;
        node.snapshot.created_at = now;
        nodes_.emplace(task.id, std::move(node));
    }

    for (const auto &task : tasks) {
        Node &node = nodes_.at(task.id);
        std::uint32_t unresolved = 0;
        for (const AgentId dependency : task.dependencies) {
            Node &predecessor = nodes_.at(dependency);
            predecessor.successors.push_back(task.id);
            if (!is_terminal(predecessor.snapshot.state) ||
                predecessor.snapshot.state != AgentState::completed) {
                ++unresolved;
            }
        }
        node.snapshot.unresolved_dependencies = unresolved;
    }

    if (graph_has_cycle_locked()) {
        nodes_ = nodes_backup;
        ready_ = ready_backup;
        if (error != nullptr) {
            *error = "task dependencies contain a cycle";
        }
        return false;
    }

    for (const auto &task : tasks) {
        Node &node = nodes_.at(task.id);
        if (node.snapshot.unresolved_dependencies == 0) {
            enqueue_ready_locked(node);
        } else {
            node.snapshot.state = AgentState::waiting_dependency;
        }
    }
    return true;
}

bool AgentScheduler::graph_has_cycle_locked() const {
    enum class Visit : std::uint8_t { none, active, done };
    std::unordered_map<AgentId, Visit> visits;

    std::function<bool(AgentId)> visit = [&](AgentId id) {
        Visit &state = visits[id];
        if (state == Visit::active) {
            return true;
        }
        if (state == Visit::done) {
            return false;
        }
        state = Visit::active;
        for (const AgentId successor : nodes_.at(id).successors) {
            if (visit(successor)) {
                return true;
            }
        }
        state = Visit::done;
        return false;
    };

    for (const auto &[id, node] : nodes_) {
        (void) node;
        if (visit(id)) {
            return true;
        }
    }
    return false;
}

void AgentScheduler::enqueue_ready_locked(Node &node) {
    node.snapshot.state = AgentState::ready;
    ready_.insert(node.snapshot.spec.id);
}

std::optional<AgentId> AgentScheduler::dispatch_locked(
    const MemorySnapshot &resources) {
    AgentId selected = kInvalidAgentId;
    std::int64_t best_score = std::numeric_limits<std::int64_t>::min();
    const auto now = std::chrono::steady_clock::now();

    for (const AgentId id : ready_) {
        Node &node = nodes_.at(id);
        const std::uint64_t requested_cpu =
            node.snapshot.spec.resources.cpu_threads;
        const std::uint64_t requested = node.snapshot.spec.resources.memory_bytes;
        const std::uint64_t allocatable_cpu =
            resources.available_cpu_threads > reserved_cpu_threads_
                ? resources.available_cpu_threads - reserved_cpu_threads_
                : 0;
        const std::uint64_t allocatable =
            resources.available_bytes > reserved_memory_bytes_
                ? resources.available_bytes - reserved_memory_bytes_
                : 0;
        if (requested_cpu > allocatable_cpu || requested > allocatable) {
            node.snapshot.state = AgentState::waiting_resource;
            continue;
        }

        if (node.snapshot.state == AgentState::waiting_resource) {
            node.snapshot.state = AgentState::ready;
        }
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - node.snapshot.created_at)
                                .count();
        const std::int64_t score =
            static_cast<std::int64_t>(node.snapshot.spec.priority) * 1'000'000LL +
            waited;
        if (selected == kInvalidAgentId || score > best_score) {
            selected = id;
            best_score = score;
        }
    }

    if (selected == kInvalidAgentId) {
        return std::nullopt;
    }
    Node &node = nodes_.at(selected);
    node.snapshot.state = AgentState::dispatched;
    reserved_cpu_threads_ += node.snapshot.spec.resources.cpu_threads;
    reserved_memory_bytes_ += node.snapshot.spec.resources.memory_bytes;
    node.resources_reserved = true;
    ready_.erase(selected);
    return selected;
}

std::optional<AgentId> AgentScheduler::try_dispatch(
    const MemorySnapshot &resources) {
    std::lock_guard lock(mutex_);
    return dispatch_locked(resources);
}

std::optional<AgentId> AgentScheduler::wait_dispatch(
    const MemorySnapshot &resources) {
    std::unique_lock lock(mutex_);
    if (ready_.empty()) {
        ready_cv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return !ready_.empty();
        });
    }
    return dispatch_locked(resources);
}

bool AgentScheduler::transition_locked(Node &node,
                                       AgentState next,
                                       std::string *error) {
    const AgentState current = node.snapshot.state;
    bool allowed = false;
    switch (next) {
        case AgentState::running:
            allowed = current == AgentState::dispatched;
            break;
        case AgentState::blocked:
            allowed = current == AgentState::running;
            break;
        case AgentState::ready:
            allowed = current == AgentState::blocked ||
                      current == AgentState::suspended ||
                      current == AgentState::waiting_resource;
            break;
        case AgentState::completed:
        case AgentState::failed:
            allowed = current == AgentState::running ||
                      current == AgentState::blocked ||
                      current == AgentState::dispatched;
            break;
        case AgentState::cancelled:
            allowed = !is_terminal(current);
            break;
        default:
            break;
    }
    if (!allowed) {
        if (error != nullptr) {
            *error = std::string("invalid transition ") + to_string(current) +
                     " -> " + to_string(next);
        }
        return false;
    }
    node.snapshot.state = next;
    return true;
}

bool AgentScheduler::mark_running(AgentId id,
                                  std::int64_t process_id,
                                  std::string *error) {
    std::lock_guard lock(mutex_);
    const auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        if (error != nullptr) *error = "agent does not exist";
        return false;
    }
    if (!transition_locked(it->second, AgentState::running, error)) {
        return false;
    }
    it->second.snapshot.process_id = process_id;
    it->second.snapshot.started_at = std::chrono::steady_clock::now();
    return true;
}

bool AgentScheduler::mark_blocked(AgentId id, std::string *error) {
    std::lock_guard lock(mutex_);
    const auto it = nodes_.find(id);
    return it != nodes_.end() &&
           transition_locked(it->second, AgentState::blocked, error);
}

bool AgentScheduler::wake(AgentId id, std::string *error) {
    std::lock_guard lock(mutex_);
    const auto it = nodes_.find(id);
    if (it == nodes_.end() ||
        !transition_locked(it->second, AgentState::ready, error)) {
        return false;
    }
    ready_.insert(id);
    ready_cv_.notify_one();
    return true;
}

bool AgentScheduler::complete(AgentId id, std::string *error) {
    std::lock_guard lock(mutex_);
    const auto it = nodes_.find(id);
    if (it == nodes_.end() ||
        !transition_locked(it->second, AgentState::completed, error)) {
        return false;
    }
    it->second.snapshot.finished_at = std::chrono::steady_clock::now();
    release_resources_locked(it->second);

    for (const AgentId successor_id : it->second.successors) {
        Node &successor = nodes_.at(successor_id);
        if (successor.snapshot.unresolved_dependencies > 0) {
            --successor.snapshot.unresolved_dependencies;
        }
        if (successor.snapshot.unresolved_dependencies == 0 &&
            successor.snapshot.state == AgentState::waiting_dependency) {
            enqueue_ready_locked(successor);
        }
    }
    ready_cv_.notify_all();
    return true;
}

bool AgentScheduler::fail(AgentId id,
                          std::string message,
                          std::string *error) {
    std::lock_guard lock(mutex_);
    const auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        if (error != nullptr) *error = "agent does not exist";
        return false;
    }
    Node &node = it->second;
    release_resources_locked(node);
    if (node.snapshot.retry_count < node.snapshot.spec.retry_limit) {
        ++node.snapshot.retry_count;
        node.snapshot.error = std::move(message);
        node.snapshot.process_id = -1;
        node.snapshot.state = AgentState::ready;
        ready_.insert(id);
        ready_cv_.notify_one();
        return true;
    }
    if (!transition_locked(node, AgentState::failed, error)) {
        return false;
    }
    node.snapshot.error = std::move(message);
    node.snapshot.finished_at = std::chrono::steady_clock::now();
    cancel_descendants_locked(id, "dependency agent failed");
    ready_cv_.notify_all();
    return true;
}

bool AgentScheduler::cancel(AgentId id, std::string *error) {
    std::lock_guard lock(mutex_);
    const auto it = nodes_.find(id);
    if (it == nodes_.end() ||
        !transition_locked(it->second, AgentState::cancelled, error)) {
        return false;
    }
    ready_.erase(id);
    release_resources_locked(it->second);
    it->second.snapshot.finished_at = std::chrono::steady_clock::now();
    cancel_descendants_locked(id, "dependency agent was cancelled");
    ready_cv_.notify_all();
    return true;
}

void AgentScheduler::release_resources_locked(Node &node) {
    if (!node.resources_reserved) {
        return;
    }
    const std::uint64_t reserved = node.snapshot.spec.resources.memory_bytes;
    const std::uint64_t reserved_cpu = node.snapshot.spec.resources.cpu_threads;
    reserved_cpu_threads_ = reserved_cpu > reserved_cpu_threads_
        ? 0
        : reserved_cpu_threads_ - reserved_cpu;
    reserved_memory_bytes_ = reserved > reserved_memory_bytes_
        ? 0
        : reserved_memory_bytes_ - reserved;
    node.resources_reserved = false;
}

void AgentScheduler::cancel_descendants_locked(AgentId id,
                                               const std::string &reason) {
    for (const AgentId successor_id : nodes_.at(id).successors) {
        Node &successor = nodes_.at(successor_id);
        if (is_terminal(successor.snapshot.state)) {
            continue;
        }
        ready_.erase(successor_id);
        release_resources_locked(successor);
        successor.snapshot.state = AgentState::cancelled;
        successor.snapshot.error = reason + ": " + std::to_string(id);
        successor.snapshot.finished_at = std::chrono::steady_clock::now();
        cancel_descendants_locked(successor_id, reason);
    }
}

bool AgentScheduler::bind_context(AgentId id,
                                  ContextId context_id,
                                  std::string *error) {
    std::lock_guard lock(mutex_);
    const auto it = nodes_.find(id);
    if (it == nodes_.end() || is_terminal(it->second.snapshot.state)) {
        if (error != nullptr) *error = "cannot bind context to agent";
        return false;
    }
    it->second.snapshot.context_id = context_id;
    return true;
}

std::optional<AgentSnapshot> AgentScheduler::snapshot(AgentId id) const {
    std::lock_guard lock(mutex_);
    const auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return std::nullopt;
    }
    return it->second.snapshot;
}

std::vector<AgentSnapshot> AgentScheduler::snapshots() const {
    std::lock_guard lock(mutex_);
    std::vector<AgentSnapshot> result;
    result.reserve(nodes_.size());
    for (const auto &[id, node] : nodes_) {
        (void) id;
        result.push_back(node.snapshot);
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.spec.id < right.spec.id;
    });
    return result;
}

std::size_t AgentScheduler::size() const {
    std::lock_guard lock(mutex_);
    return nodes_.size();
}

bool AgentScheduler::all_terminal() const {
    std::lock_guard lock(mutex_);
    return std::all_of(nodes_.begin(), nodes_.end(), [](const auto &entry) {
        return is_terminal(entry.second.snapshot.state);
    });
}

void AgentScheduler::notify() {
    ready_cv_.notify_all();
}

}  // namespace agent_runtime
