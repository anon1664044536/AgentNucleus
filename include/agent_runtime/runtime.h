#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "agent_runtime/cgroup_manager.h"
#include "agent_runtime/process_executor.h"
#include "agent_runtime/scheduler.h"
#include "agent_runtime/shared_memory.h"

namespace agent_runtime {

struct AgentExecutionResult {
    bool success{true};
    std::string message;
    SharedMemoryRegion output;
    std::size_t output_length{0};
    SharedDataType output_type{SharedDataType::binary};
    std::uint32_t output_flags{shared_buffer_none};
};

using AgentHandler = std::function<AgentExecutionResult(const AgentSnapshot &)>;

struct RuntimeConfig {
    std::size_t worker_count{1};
    bool enable_cgroups{false};
    std::string cgroup_root{"/sys/fs/cgroup/agent-runtime"};
    std::size_t max_agent_output_bytes{1024U * 1024U};
};

class AgentRuntime {
public:
    explicit AgentRuntime(std::size_t worker_count = 1);
    explicit AgentRuntime(RuntimeConfig config);
    ~AgentRuntime();

    AgentRuntime(const AgentRuntime &) = delete;
    AgentRuntime &operator=(const AgentRuntime &) = delete;

    void register_handler(std::string kind, AgentHandler handler);
    bool start(std::string *error = nullptr);
    void stop();

    bool submit(const AgentTaskSpec &task, std::string *error = nullptr);
    bool submit_batch(const std::vector<AgentTaskSpec> &tasks,
                      std::string *error = nullptr);
    bool cancel(AgentId id, std::string *error = nullptr);
    bool wait_until_idle(std::chrono::milliseconds timeout);
    std::optional<SharedResultHandle> result(
        AgentId id,
        std::string *error = nullptr) const;
    bool release_result(AgentId id, std::string *error = nullptr);

    AgentScheduler &scheduler() noexcept;
    const AgentScheduler &scheduler() const noexcept;

private:
    void worker_loop();
    AgentExecutionResult execute(const AgentSnapshot &snapshot);

    RuntimeConfig config_;
    AgentScheduler scheduler_;
    ResourceMonitor monitor_;
    ProcessExecutor process_executor_;
    AgentResultStore result_store_;
    CgroupManager cgroup_manager_;
    std::unordered_map<std::string, AgentHandler> handlers_;
    std::mutex handlers_mutex_;
    std::unordered_map<AgentId, std::shared_ptr<std::atomic_bool>> cancel_tokens_;
    std::mutex cancel_mutex_;
    std::vector<std::thread> workers_;
    std::atomic_size_t active_executions_{0};
    std::atomic_bool running_{false};
    std::atomic_bool cancel_requested_{false};
};

}  // namespace agent_runtime
