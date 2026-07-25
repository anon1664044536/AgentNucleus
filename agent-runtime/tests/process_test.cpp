#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "agent_runtime/runtime.h"

namespace ar = agent_runtime;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << "check failed at line " << __LINE__ << ": "          \
                      << #condition << '\n';                                    \
            return 1;                                                           \
        }                                                                       \
    } while (false)

int main() {
    ar::RuntimeConfig config;
    config.worker_count = 1;
    config.max_agent_output_bytes = 8;
    ar::AgentRuntime runtime(config);
    std::string error;
    runtime.register_handler("tool", [](const ar::AgentSnapshot &) {
        ar::AgentExecutionResult result;
        result.output = ar::SharedMemoryRegion::create(32);
        constexpr char payload[] = "{\"x\":1}";
        std::memcpy(result.output.data(), payload, sizeof(payload) - 1);
        result.output_length = sizeof(payload) - 1;
        result.output_type = ar::SharedDataType::json_utf8;
        return result;
    });
    CHECK(runtime.start(&error));

    ar::AgentTaskSpec task;
    task.id = 10;
    task.name = "process-agent";
    task.command = {"/bin/sh", "-c", "printf 'hello'; printf 'err' >&2"};
    task.resources.timeout = std::chrono::seconds(2);
    CHECK(runtime.submit(task, &error));
    CHECK(runtime.wait_until_idle(std::chrono::seconds(5)));

    const auto snapshot = runtime.scheduler().snapshot(10);
    CHECK(snapshot.has_value());
    CHECK(snapshot->state == ar::AgentState::completed);
    auto first_result = runtime.result(10, &error);
    CHECK(first_result.has_value());
    auto first_region = ar::SharedMemoryRegion::map_existing_read_only(
        first_result->release_descriptor(),
        first_result->reference.region_size,
        first_result->reference.region_id,
        &error);
    CHECK(first_region.valid());
    CHECK(first_result->reference.length == 8);
    CHECK(first_result->reference.region_size == 8);
    CHECK(std::string(static_cast<const char *>(first_region.data()), 8) ==
          "helloerr");
    CHECK((first_result->reference.flags & ar::shared_buffer_truncated) == 0);
    CHECK((first_result->reference.flags & ar::shared_buffer_immutable) != 0);
    CHECK((first_result->reference.flags & ar::shared_buffer_stdout_stderr) != 0);

    ar::AgentTaskSpec truncated;
    truncated.id = 12;
    truncated.name = "truncated-output-agent";
    truncated.command = {"/bin/sh", "-c", "printf '1234567890'"};
    truncated.resources.timeout = std::chrono::seconds(2);
    CHECK(runtime.submit(truncated, &error));
    CHECK(runtime.wait_until_idle(std::chrono::seconds(5)));
    auto truncated_result = runtime.result(12, &error);
    CHECK(truncated_result.has_value());
    CHECK(truncated_result->reference.length == 8);
    CHECK((truncated_result->reference.flags & ar::shared_buffer_truncated) != 0);

    ar::AgentTaskSpec tool;
    tool.id = 13;
    tool.name = "tool-result-agent";
    tool.kind = "tool";
    CHECK(runtime.submit(tool, &error));
    CHECK(runtime.wait_until_idle(std::chrono::seconds(5)));
    auto tool_result = runtime.result(13, &error);
    CHECK(tool_result.has_value());
    CHECK(tool_result->reference.data_type ==
          static_cast<std::uint32_t>(ar::SharedDataType::json_utf8));
    CHECK(tool_result->reference.length == 7);
    CHECK(runtime.release_result(13, &error));
    CHECK(!runtime.result(13, &error).has_value());

    ar::AgentTaskSpec cancelled;
    cancelled.id = 11;
    cancelled.name = "cancelled-process-agent";
    cancelled.command = {"/bin/sh", "-c", "sleep 5"};
    cancelled.resources.timeout = std::chrono::seconds(10);
    CHECK(runtime.submit(cancelled, &error));
    const auto running_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        const auto state = runtime.scheduler().snapshot(11);
        CHECK(state.has_value());
        if (state->state == ar::AgentState::running) break;
        CHECK(std::chrono::steady_clock::now() < running_deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(runtime.cancel(11, &error));
    CHECK(runtime.wait_until_idle(std::chrono::seconds(2)));
    CHECK(runtime.scheduler().snapshot(11)->state == ar::AgentState::cancelled);
    CHECK(runtime.result(11, &error).has_value());
    runtime.stop();
    std::cout << "process tests passed\n";
    return 0;
}
