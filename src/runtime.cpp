#include "agent_runtime/runtime.h"

#include <chrono>
#include <exception>
#include <limits>
#include <utility>

namespace agent_runtime {

AgentRuntime::AgentRuntime(std::size_t worker_count)
    : AgentRuntime(RuntimeConfig{.worker_count = worker_count}) {}

AgentRuntime::AgentRuntime(RuntimeConfig config)
    : config_(std::move(config)), cgroup_manager_(config_.cgroup_root) {
    if (config_.worker_count == 0) {
        config_.worker_count = 1;
    }
}

AgentRuntime::~AgentRuntime() {
    stop();
}

void AgentRuntime::register_handler(std::string kind, AgentHandler handler) {
    std::lock_guard lock(handlers_mutex_);
    handlers_[std::move(kind)] = std::move(handler);
}

bool AgentRuntime::start(std::string *error) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        if (error != nullptr) *error = "runtime is already running";
        return false;
    }
    if (config_.enable_cgroups && !cgroup_manager_.initialize(error)) {
        running_.store(false);
        return false;
    }
    cancel_requested_.store(false);
    try {
        workers_.reserve(config_.worker_count);
        for (std::size_t index = 0; index < config_.worker_count; ++index) {
            workers_.emplace_back(&AgentRuntime::worker_loop, this);
        }
    } catch (const std::exception &exception) {
        running_.store(false);
        cancel_requested_.store(true);
        scheduler_.notify();
        for (auto &worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        workers_.clear();
        if (error != nullptr) *error = exception.what();
        return false;
    }
    return true;
}

void AgentRuntime::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    cancel_requested_.store(true);
    {
        std::lock_guard lock(cancel_mutex_);
        for (const auto &[id, token] : cancel_tokens_) {
            (void) id;
            token->store(true);
        }
    }
    scheduler_.notify();
    for (auto &worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
}

bool AgentRuntime::submit(const AgentTaskSpec &task, std::string *error) {
    return scheduler_.submit(task, error);
}

bool AgentRuntime::submit_batch(const std::vector<AgentTaskSpec> &tasks,
                                std::string *error) {
    return scheduler_.submit_batch(tasks, error);
}

bool AgentRuntime::cancel(AgentId id, std::string *error) {
    {
        std::lock_guard lock(cancel_mutex_);
        const auto it = cancel_tokens_.find(id);
        if (it != cancel_tokens_.end()) {
            it->second->store(true);
        }
    }
    return scheduler_.cancel(id, error);
}

bool AgentRuntime::wait_until_idle(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (scheduler_.all_terminal()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return scheduler_.all_terminal();
}

AgentScheduler &AgentRuntime::scheduler() noexcept {
    return scheduler_;
}

const AgentScheduler &AgentRuntime::scheduler() const noexcept {
    return scheduler_;
}

void AgentRuntime::worker_loop() {
    while (running_.load()) {
        MemorySnapshot resources;
        if (const auto sample = monitor_.sample(); sample.has_value()) {
            resources = *sample;
        } else {
            resources.available_bytes = std::numeric_limits<std::uint64_t>::max();
        }

        const auto id = scheduler_.wait_dispatch(resources);
        if (!running_.load()) {
            break;
        }
        if (!id.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const auto task = scheduler_.snapshot(*id);
        if (!task.has_value()) {
            continue;
        }

        const AgentExecutionResult result = execute(*task);
        std::string ignored;
        if (result.success) {
            scheduler_.complete(*id, &ignored);
        } else {
            scheduler_.fail(*id, result.message, &ignored);
        }
    }
}

AgentExecutionResult AgentRuntime::execute(const AgentSnapshot &snapshot) {
    if (!snapshot.spec.command.empty()) {
        auto cancel_token = std::make_shared<std::atomic_bool>(
            cancel_requested_.load());
        {
            std::lock_guard lock(cancel_mutex_);
            cancel_tokens_[snapshot.spec.id] = cancel_token;
        }
        const auto remove_cancel_token = [&] {
            std::lock_guard lock(cancel_mutex_);
            cancel_tokens_.erase(snapshot.spec.id);
        };

        bool cgroup_created = false;
        if (config_.enable_cgroups) {
            CgroupLimits limits;
            limits.cpu_quota_us =
                static_cast<std::uint64_t>(snapshot.spec.resources.cpu_threads) *
                limits.cpu_period_us;
            limits.memory_max_bytes = snapshot.spec.resources.memory_bytes;
            std::string cgroup_error;
            if (!cgroup_manager_.create_agent_group(
                    snapshot.spec.id, limits, &cgroup_error)) {
                remove_cancel_token();
                return {false, cgroup_error};
            }
            cgroup_created = true;
        }

        std::string attach_error;
        const ProcessResult result = process_executor_.run(
            snapshot.spec.command,
            snapshot.spec.resources.timeout,
            cancel_token.get(),
            [&](std::int64_t process_id, std::string *start_error) {
                if (cgroup_created) {
                    if (!cgroup_manager_.attach(
                            snapshot.spec.id, process_id, &attach_error)) {
                        if (start_error != nullptr) *start_error = attach_error;
                        return false;
                    }
                }
                std::string ignored;
                if (!scheduler_.mark_running(
                        snapshot.spec.id, process_id, &ignored)) {
                    if (start_error != nullptr) *start_error = ignored;
                    return false;
                }
                return true;
            });
        if (cgroup_created) {
            std::string ignored;
            (void) cgroup_manager_.kill_all(snapshot.spec.id, &ignored);
            (void) cgroup_manager_.remove_agent_group(snapshot.spec.id, &ignored);
        }
        remove_cancel_token();
        if (!result.started) {
            return {false, result.error};
        }
        if (result.timed_out) {
            return {false, "agent process timed out"};
        }
        if (result.exit_code != 0) {
            return {false,
                    "agent process exited with code " +
                        std::to_string(result.exit_code)};
        }
        return {};
    }

    AgentHandler handler;
    {
        std::lock_guard lock(handlers_mutex_);
        const auto it = handlers_.find(snapshot.spec.kind);
        if (it == handlers_.end()) {
            return {false, "no handler registered for kind: " + snapshot.spec.kind};
        }
        handler = it->second;
    }
    std::string ignored;
    if (!scheduler_.mark_running(snapshot.spec.id, -1, &ignored)) {
        return {false, ignored};
    }
    try {
        return handler(snapshot);
    } catch (const std::exception &exception) {
        return {false, exception.what()};
    } catch (...) {
        return {false, "agent handler raised an unknown exception"};
    }
}

}  // namespace agent_runtime
