#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "agent_runtime/invocation_channel.h"
#include "top_runtime.h"

namespace ar = agent_runtime;
namespace art = agent_runtime_top;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << "check failed at line " << __LINE__ << ": "          \
                      << #condition << '\n';                                    \
            passed = false;                                                     \
        }                                                                       \
    } while (false)

int main() {
    bool passed = true;
    art::TopRuntimeConfig config;
    config.model_path = "simulated.gguf";
    config.model_alias = "integration-model";
    config.max_contexts = 4;

    art::TopRuntime runtime(config);
    std::string error;
    if (!runtime.initialize(&error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::string socket_path =
        "/tmp/agent-top-integration-" + std::to_string(getpid()) + ".sock";
    ar::InvocationServer server(
        [&](const ar::InvocationRequest &request,
            const std::vector<ar::InvocationMappedInput> &inputs,
            const std::function<bool()> &cancel_requested) {
            return runtime.handle(request, inputs, cancel_requested);
        },
        socket_path);
    if (!server.start(&error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::atomic_bool stop_server{false};
    std::thread server_thread(
        [&] { server.serve([&] { return stop_server.load(); }); });
    ar::InvocationClient client(socket_path);

    ar::InvocationRequest first;
    first.agent_id = 1000;
    first.kind = ar::InvocationKind::model;
    first.target = "integration-model";
    first.operation = "generate";
    first.payload =
        R"({"system_prompt":"shared","prompt":"first","max_tokens":4})";
    first.resources.timeout = std::chrono::seconds(2);

    ar::InvocationCallResult first_result;
    CHECK(client.invoke(first, {}, &first_result,
                        std::chrono::seconds(2), nullptr, &error));
    CHECK(first_result.response.status == ar::InvocationStatus::ok);
    CHECK(first_result.response.context_id != 0);
    CHECK(first_result.output.valid());
    CHECK(!first_result.response.metrics.cache_hit);

    ar::InvocationRequest second = first;
    second.agent_id = 1001;
    second.payload =
        R"({"system_prompt":"shared","prompt":"second","max_tokens":4})";
    ar::InvocationCallResult second_result;
    CHECK(client.invoke(second, {}, &second_result,
                        std::chrono::seconds(2), nullptr, &error));
    CHECK(second_result.response.status == ar::InvocationStatus::ok);
    CHECK(second_result.response.context_id !=
          first_result.response.context_id);
    CHECK(second_result.response.metrics.cache_hit);
    CHECK(second_result.response.metrics.reused_tokens > 0);

    ar::InvocationRequest tool;
    tool.agent_id = 2000;
    tool.kind = ar::InvocationKind::tool;
    tool.target = "echo";
    tool.operation = "execute";
    tool.payload = "through-memfd";
    tool.resources.timeout = std::chrono::seconds(2);
    ar::InvocationCallResult tool_result;
    CHECK(client.invoke(tool, {}, &tool_result,
                        std::chrono::seconds(2), nullptr, &error));
    CHECK(tool_result.response.status == ar::InvocationStatus::ok);
    CHECK(tool_result.output.valid());
    if (tool_result.output.valid()) {
        const auto &reference = tool_result.response.output;
        const std::string output(
            static_cast<const char *>(tool_result.output.data()) +
                reference.offset,
            static_cast<std::size_t>(reference.length));
        CHECK(output == "through-memfd");
    }

    stop_server.store(true);
    server_thread.join();
    runtime.shutdown();
    if (!passed) return 1;
    std::cout << "top invocation integration tests passed\n";
    return 0;
}
