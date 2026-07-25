#include <iostream>
#include <string>

#include "top_runtime.h"

namespace ar = agent_runtime;
namespace art = agent_runtime_top;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << "check failed at line " << __LINE__ << ": "          \
                      << #condition << '\n';                                    \
            return 1;                                                           \
        }                                                                       \
    } while (false)

int main() {
    art::TopRuntimeConfig config;
    config.model_path = "simulated.gguf";
    config.model_alias = "test";
    config.max_contexts = 1;
    config.max_swapped_contexts = 2;
    config.enable_context_swap = true;
    art::TopRuntime runtime(config);
    std::string error;
    CHECK(runtime.initialize(&error));

    ar::InvocationRequest warmup;
    warmup.agent_id = 50;
    warmup.kind = ar::InvocationKind::model;
    warmup.target = "test";
    warmup.operation = "warmup_prefix";
    warmup.payload = R"({"system_prompt":"preloaded-policy"})";
    auto warmed = runtime.handle(warmup, {}, [] { return false; });
    CHECK(warmed.status == ar::InvocationStatus::ok);

    ar::InvocationRequest request;
    request.agent_id = 100;
    request.kind = ar::InvocationKind::model;
    request.target = "test";
    request.operation = "generate";
    request.payload =
        R"({"system_prompt":"shared-policy","prompt":"hello","max_tokens":8})";
    auto result = runtime.handle(request, {}, [] { return false; });
    CHECK(result.status == ar::InvocationStatus::ok);
    CHECK(result.context_id != 0);
    CHECK(result.output.valid());
    CHECK(result.output_length > 0);
    CHECK(!result.metrics.cache_hit);
    if (result.output.valid()) {
        const std::string generated(
            static_cast<const char *>(result.output.data()),
            result.output_length);
        CHECK(generated.find("<|user|>") != std::string::npos);
    }

    ar::InvocationRequest continuation = request;
    continuation.agent_id = 101;
    continuation.parent_id = 100;
    continuation.context_id = result.context_id;
    continuation.payload = R"({"prompt":"continue"})";
    auto continued =
        runtime.handle(continuation, {}, [] { return false; });
    CHECK(continued.status == ar::InvocationStatus::ok);
    CHECK(continued.context_id == result.context_id);

    ar::InvocationRequest second_agent = request;
    second_agent.agent_id = 102;
    second_agent.payload =
        R"({"system_prompt":"shared-policy","prompt":"independent"})";
    auto second_result =
        runtime.handle(second_agent, {}, [] { return false; });
    CHECK(second_result.status == ar::InvocationStatus::ok);
    CHECK(second_result.context_id != result.context_id);
    CHECK(second_result.metrics.cache_hit);
    CHECK(second_result.metrics.reused_tokens > 0);

    continuation.payload = R"({"prompt":"restore previous context"})";
    auto restored =
        runtime.handle(continuation, {}, [] { return false; });
    CHECK(restored.status == ar::InvocationStatus::ok);
    CHECK(restored.context_id == result.context_id);

    ar::InvocationRequest release;
    release.agent_id = 101;
    release.parent_id = 100;
    release.kind = ar::InvocationKind::model;
    release.target = "test";
    release.operation = "release_context";
    release.context_id = result.context_id;
    auto released = runtime.handle(release, {}, [] { return false; });
    CHECK(released.status == ar::InvocationStatus::ok);

    release.agent_id = 100;
    release.parent_id = 0;
    released = runtime.handle(release, {}, [] { return false; });
    CHECK(released.status == ar::InvocationStatus::ok);

    release.agent_id = 102;
    release.context_id = second_result.context_id;
    released = runtime.handle(release, {}, [] { return false; });
    CHECK(released.status == ar::InvocationStatus::ok);

    for (int index = 0; index < 6; ++index) {
        warmup.agent_id = static_cast<ar::AgentId>(60 + index);
        warmup.payload =
            "{\"system_prompt\":\"prefix-" +
            std::to_string(index) + "\"}";
        warmed = runtime.handle(warmup, {}, [] { return false; });
        CHECK(warmed.status == ar::InvocationStatus::ok);
    }

    // Refresh the oldest prefix, then force one eviction. The refreshed
    // prefix must survive if warmup hits update the LRU clock.
    warmup.agent_id = 70;
    warmup.payload =
        R"({"system_prompt":"preloaded-policy"})";
    warmed = runtime.handle(warmup, {}, [] { return false; });
    CHECK(warmed.status == ar::InvocationStatus::ok);
    warmup.agent_id = 71;
    warmup.payload = R"({"system_prompt":"prefix-6"})";
    warmed = runtime.handle(warmup, {}, [] { return false; });
    CHECK(warmed.status == ar::InvocationStatus::ok);

    ar::InvocationRequest lru_probe = request;
    lru_probe.agent_id = 103;
    lru_probe.context_id = 0;
    lru_probe.payload =
        R"({"system_prompt":"preloaded-policy","prompt":"probe"})";
    auto lru_result =
        runtime.handle(lru_probe, {}, [] { return false; });
    CHECK(lru_result.status == ar::InvocationStatus::ok);
    CHECK(lru_result.metrics.cache_hit);

    release.agent_id = 103;
    release.context_id = lru_result.context_id;
    released = runtime.handle(release, {}, [] { return false; });
    CHECK(released.status == ar::InvocationStatus::ok);

    for (int index = 7; index < 10; ++index) {
        warmup.agent_id = static_cast<ar::AgentId>(72 + index);
        warmup.payload =
            "{\"system_prompt\":\"prefix-" +
            std::to_string(index) + "\"}";
        warmed = runtime.handle(warmup, {}, [] { return false; });
        CHECK(warmed.status == ar::InvocationStatus::ok);
    }

    ar::InvocationRequest statistics;
    statistics.agent_id = 300;
    statistics.kind = ar::InvocationKind::model;
    statistics.target = "test";
    statistics.operation = "stats";
    auto stats = runtime.handle(statistics, {}, [] { return false; });
    CHECK(stats.status == ar::InvocationStatus::ok);
    CHECK(stats.output.valid());
    if (stats.output.valid()) {
        const std::string stats_text(
            static_cast<const char *>(stats.output.data()),
            stats.output_length);
        CHECK(stats_text.find("\"prefix_evictions\":0") ==
              std::string::npos);
        CHECK(stats_text.find("\"swap_outs\":0") ==
              std::string::npos);
        CHECK(stats_text.find("\"snapshot_count\":0") !=
              std::string::npos);
    }

    ar::InvocationRequest tool;
    tool.agent_id = 200;
    tool.kind = ar::InvocationKind::tool;
    tool.target = "echo";
    tool.operation = "execute";
    tool.payload = "tool-output";
    auto tool_result = runtime.handle(tool, {}, [] { return false; });
    CHECK(tool_result.status == ar::InvocationStatus::ok);
    CHECK(tool_result.output_length == std::string("tool-output").size());

    auto cancelled = runtime.handle(request, {}, [] { return true; });
    CHECK(cancelled.status == ar::InvocationStatus::cancelled);
    runtime.shutdown();
    std::cout << "top runtime tests passed\n";
    return 0;
}
