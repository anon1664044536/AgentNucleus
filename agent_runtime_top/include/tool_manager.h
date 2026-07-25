#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "agent_runtime/invocation_channel.h"

namespace agent_runtime_top {

struct ToolResult {
    agent_runtime::InvocationStatus status{
        agent_runtime::InvocationStatus::ok};
    std::string message;
    std::string output;
    agent_runtime::SharedDataType output_type{
        agent_runtime::SharedDataType::text_utf8};
    std::int64_t process_id{-1};
    std::uint64_t execution_time_us{0};
    bool output_truncated{false};
};

using ToolHandler = std::function<ToolResult(
    const agent_runtime::InvocationRequest &,
    const std::vector<agent_runtime::InvocationMappedInput> &,
    const std::function<bool()> &cancel_requested)>;

class ToolManager {
public:
    explicit ToolManager(bool enable_shell_tool = false);

    bool register_tool(std::string name,
                       ToolHandler handler,
                       std::string *error = nullptr);
    ToolResult execute(
        const agent_runtime::InvocationRequest &request,
        const std::vector<agent_runtime::InvocationMappedInput> &inputs,
        const std::function<bool()> &cancel_requested) const;
    std::vector<std::string> list() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ToolHandler> tools_;
};

}  // namespace agent_runtime_top
