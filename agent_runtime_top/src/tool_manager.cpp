#include "tool_manager.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "agent_runtime/process_executor.h"

namespace agent_runtime_top {
namespace {

constexpr std::size_t kMaxToolOutputBytes = 1024U * 1024U;

std::string input_text(
    const agent_runtime::InvocationMappedInput &input) {
    const auto *begin =
        static_cast<const char *>(input.region.data()) +
        static_cast<std::size_t>(input.reference.offset);
    return std::string(
        begin, static_cast<std::size_t>(input.reference.length));
}

ToolResult echo_tool(
    const agent_runtime::InvocationRequest &request,
    const std::vector<agent_runtime::InvocationMappedInput> &inputs,
    const std::function<bool()> &cancel_requested) {
    ToolResult result;
    if (request.operation != "execute") {
        result.status = agent_runtime::InvocationStatus::rejected;
        result.message = "echo tool only supports operation=execute";
        return result;
    }
    if (cancel_requested()) {
        result.status = agent_runtime::InvocationStatus::cancelled;
        result.message = "echo tool cancelled";
        return result;
    }
    if (request.payload.empty() && !inputs.empty()) {
        if (inputs.front().reference.length > kMaxToolOutputBytes) {
            result.status = agent_runtime::InvocationStatus::rejected;
            result.message = "echo output exceeds size limit";
            return result;
        }
        result.output = input_text(inputs.front());
    } else {
        result.output = request.payload;
    }
    if (result.output.size() > kMaxToolOutputBytes) {
        result.status = agent_runtime::InvocationStatus::rejected;
        result.message = "echo output exceeds size limit";
        result.output.clear();
    }
    return result;
}

ToolResult concat_tool(
    const agent_runtime::InvocationRequest &request,
    const std::vector<agent_runtime::InvocationMappedInput> &inputs,
    const std::function<bool()> &cancel_requested) {
    ToolResult result;
    if (request.operation != "execute") {
        result.status = agent_runtime::InvocationStatus::rejected;
        result.message = "concat tool only supports operation=execute";
        return result;
    }
    result.output = request.payload;
    if (result.output.size() > kMaxToolOutputBytes) {
        result.status = agent_runtime::InvocationStatus::rejected;
        result.message = "combined tool output exceeds size limit";
        result.output.clear();
        return result;
    }
    for (const auto &input : inputs) {
        if (cancel_requested()) {
            result.status = agent_runtime::InvocationStatus::cancelled;
            result.message = "concat tool cancelled";
            result.output.clear();
            return result;
        }
        if (input.reference.length >
                kMaxToolOutputBytes - result.output.size()) {
            result.status = agent_runtime::InvocationStatus::rejected;
            result.message = "combined tool input exceeds size limit";
            result.output.clear();
            return result;
        }
        result.output.append(input_text(input));
    }
    return result;
}

ToolResult shell_tool(
    const agent_runtime::InvocationRequest &request,
    const std::vector<agent_runtime::InvocationMappedInput> &,
    const std::function<bool()> &cancel_requested) {
    ToolResult result;
    if (request.operation != "execute" || request.payload.empty()) {
        result.status = agent_runtime::InvocationStatus::rejected;
        result.message =
            "shell tool requires operation=execute and a command payload";
        return result;
    }

    std::vector<char> output(kMaxToolOutputBytes);
    std::atomic_bool cancelled{cancel_requested()};
    std::jthread cancellation_watcher(
        [&](std::stop_token stop_token) {
            while (!stop_token.stop_requested() && !cancelled.load()) {
                if (cancel_requested()) {
                    cancelled.store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

    agent_runtime::ProcessExecutor executor;
    const auto process_result = executor.run(
        {"/bin/sh", "-c", request.payload},
        request.resources.timeout,
        &cancelled,
        {},
        output.data(),
        output.size());
    cancellation_watcher.request_stop();
    cancellation_watcher.join();

    result.process_id = process_result.process_id;
    result.output.assign(output.data(), process_result.output_bytes);
    result.output_truncated = process_result.output_truncated;
    if (!process_result.started) {
        result.status = agent_runtime::InvocationStatus::failed;
        result.message = process_result.error;
    } else if (cancelled.load()) {
        result.status = agent_runtime::InvocationStatus::cancelled;
        result.message = "shell tool cancelled";
    } else if (process_result.timed_out) {
        result.status = agent_runtime::InvocationStatus::failed;
        result.message = "shell tool timed out";
    } else if (process_result.exit_code != 0) {
        result.status = agent_runtime::InvocationStatus::failed;
        result.message =
            "shell tool exited with code " +
            std::to_string(process_result.exit_code);
    }
    return result;
}

}  // namespace

ToolManager::ToolManager(bool enable_shell_tool) {
    std::string ignored;
    (void) register_tool("echo", echo_tool, &ignored);
    (void) register_tool("concat", concat_tool, &ignored);
    if (enable_shell_tool) {
        (void) register_tool("shell", shell_tool, &ignored);
    }
}

bool ToolManager::register_tool(std::string name,
                                ToolHandler handler,
                                std::string *error) {
    if (error != nullptr) error->clear();
    if (name.empty() || !handler) {
        if (error != nullptr) *error = "tool name and handler are required";
        return false;
    }
    std::lock_guard lock(mutex_);
    if (tools_.contains(name)) {
        if (error != nullptr) *error = "tool is already registered";
        return false;
    }
    tools_.emplace(std::move(name), std::move(handler));
    return true;
}

ToolResult ToolManager::execute(
    const agent_runtime::InvocationRequest &request,
    const std::vector<agent_runtime::InvocationMappedInput> &inputs,
    const std::function<bool()> &cancel_requested) const {
    ToolHandler handler;
    {
        std::lock_guard lock(mutex_);
        const auto found = tools_.find(request.target);
        if (found == tools_.end()) {
            ToolResult result;
            result.status = agent_runtime::InvocationStatus::rejected;
            result.message = "tool is not registered: " + request.target;
            return result;
        }
        handler = found->second;
    }
    const auto started = std::chrono::steady_clock::now();
    ToolResult result = handler(request, inputs, cancel_requested);
    if (result.execution_time_us == 0) {
        result.execution_time_us =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count());
    }
    return result;
}

std::vector<std::string> ToolManager::list() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto &[name, handler] : tools_) {
        (void) handler;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace agent_runtime_top
