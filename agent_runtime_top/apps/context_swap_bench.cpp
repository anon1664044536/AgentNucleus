#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "top_runtime.h"

namespace ar = agent_runtime;
namespace art = agent_runtime_top;

namespace {

void usage() {
    std::cout
        << "usage: agent_top_context_bench [OPTIONS]\n"
        << "  --model PATH        GGUF model path\n"
        << "  --threads N         CPU threads (default: 4)\n"
        << "  --ctx-tokens N      llama KV token capacity (default: 2048)\n"
        << "  --batch-tokens N    prompt decode batch size (default: 256)\n"
        << "  --max-tokens N      visible tokens per turn (default: 4)\n"
        << "  --swap-dir PATH     checkpoint parent directory\n"
        << "  --kv-type TYPE      f16, q8_0, or q4_0\n";
}

template <typename T>
bool parse_unsigned(const std::string &text, T *value) {
    static_assert(std::is_unsigned_v<T>);
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), *value);
    return !text.empty() && parsed.ec == std::errc{} &&
           parsed.ptr == text.data() + text.size();
}

bool parse_kv_type(const std::string &text, llm_kv_type *type) {
    if (text == "f16") {
        *type = LLM_KV_TYPE_F16;
    } else if (text == "q8_0") {
        *type = LLM_KV_TYPE_Q8_0;
    } else if (text == "q4_0") {
        *type = LLM_KV_TYPE_Q4_0;
    } else {
        return false;
    }
    return true;
}

struct TimedResult {
    ar::InvocationHandlerResult result;
    std::uint64_t end_to_end_us{0};
};

TimedResult timed_handle(art::TopRuntime *runtime,
                         const ar::InvocationRequest &request) {
    const auto started = std::chrono::steady_clock::now();
    auto result = runtime->handle(request, {}, [] { return false; });
    const auto finished = std::chrono::steady_clock::now();
    return {
        .result = std::move(result),
        .end_to_end_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                finished - started)
                .count())};
}

std::string mapped_text(const ar::InvocationHandlerResult &result) {
    if (!result.output.valid() || result.output_length == 0) return {};
    return std::string(
        static_cast<const char *>(result.output.data()),
        result.output_length);
}

bool release_context(art::TopRuntime *runtime,
                     ar::AgentId agent_id,
                     art::ContextId context_id) {
    ar::InvocationRequest release;
    release.agent_id = agent_id;
    release.kind = ar::InvocationKind::model;
    release.target = "swap-bench";
    release.operation = "release_context";
    release.context_id = context_id;
    const auto result =
        runtime->handle(release, {}, [] { return false; });
    if (result.status != ar::InvocationStatus::ok) {
        std::cerr << "cannot release context " << context_id << ": "
                  << result.message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    art::TopRuntimeConfig config;
    config.model_alias = "swap-bench";
    config.context_tokens = 2048;
    config.batch_tokens = 256;
    config.max_tokens = 4;
    config.max_contexts = 1;
    config.max_swapped_contexts = 2;
    config.enable_context_swap = true;
    // This benchmark isolates sequence checkpoint overhead. Chat-template
    // correctness is covered independently by TopRuntime tests.
    config.use_chat_template = false;

    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help" || option == "-h") {
            usage();
            return 0;
        }
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << option << '\n';
            return 2;
        }
        const std::string value = argv[++index];
        if (option == "--model") {
            config.model_path = value;
        } else if (option == "--threads") {
            if (!parse_unsigned(value, &config.cpu_threads) ||
                config.cpu_threads == 0) {
                std::cerr << "invalid thread count\n";
                return 2;
            }
        } else if (option == "--ctx-tokens") {
            if (!parse_unsigned(value, &config.context_tokens) ||
                config.context_tokens == 0) {
                std::cerr << "invalid context token capacity\n";
                return 2;
            }
        } else if (option == "--batch-tokens") {
            if (!parse_unsigned(value, &config.batch_tokens) ||
                config.batch_tokens == 0) {
                std::cerr << "invalid batch token count\n";
                return 2;
            }
        } else if (option == "--max-tokens") {
            if (!parse_unsigned(value, &config.max_tokens) ||
                config.max_tokens == 0) {
                std::cerr << "invalid generated token limit\n";
                return 2;
            }
        } else if (option == "--swap-dir") {
            config.swap_directory = value;
        } else if (option == "--kv-type") {
            if (!parse_kv_type(value, &config.kv_type_k)) {
                std::cerr << "invalid KV type\n";
                return 2;
            }
            config.kv_type_v = config.kv_type_k;
        } else {
            std::cerr << "unknown option: " << option << '\n';
            return 2;
        }
    }

#ifdef AGENT_RUNTIME_TOP_WITH_LLAMA
    if (config.model_path.empty()) {
        std::cerr << "--model is required when llama.cpp is enabled\n";
        return 2;
    }
#else
    if (config.model_path.empty()) config.model_path = "simulated.gguf";
#endif

    art::TopRuntime runtime(config);
    std::string error;
    if (!runtime.initialize(&error)) {
        std::cerr << error << '\n';
        return 1;
    }

    auto make_generate = [&](ar::AgentId agent_id,
                             art::ContextId context_id,
                             const std::string &prompt) {
        ar::InvocationRequest request;
        request.agent_id = agent_id;
        request.kind = ar::InvocationKind::model;
        request.target = "swap-bench";
        request.operation = "generate";
        request.context_id = context_id;
        request.payload =
            "{\"prompt\":\"" + prompt + "\",\"max_tokens\":" +
            std::to_string(config.max_tokens) + "}";
        request.resources.cpu_threads = config.cpu_threads;
        return request;
    };

    const auto first =
        timed_handle(&runtime, make_generate(100, 0, "Say A."));
    if (first.result.status != ar::InvocationStatus::ok) {
        std::cerr << "first context failed: " << first.result.message << '\n';
        return 1;
    }
    const auto second =
        timed_handle(&runtime, make_generate(101, 0, "Say B."));
    if (second.result.status != ar::InvocationStatus::ok) {
        std::cerr << "swap-out request failed: "
                  << second.result.message << '\n';
        return 1;
    }
    const auto restored = timed_handle(
        &runtime,
        make_generate(100, first.result.context_id, "Continue A."));
    if (restored.result.status != ar::InvocationStatus::ok) {
        std::cerr << "swap-in request failed: "
                  << restored.result.message << '\n';
        return 1;
    }

    ar::InvocationRequest statistics;
    statistics.agent_id = 999;
    statistics.kind = ar::InvocationKind::model;
    statistics.target = "swap-bench";
    statistics.operation = "stats";
    const auto stats =
        runtime.handle(statistics, {}, [] { return false; });
    const std::string stats_text = mapped_text(stats);
    if (stats.status != ar::InvocationStatus::ok ||
        stats_text.find("\"swap_outs\":2") == std::string::npos ||
        stats_text.find("\"swap_ins\":1") == std::string::npos) {
        std::cerr << "unexpected swap counters: " << stats_text << '\n';
        return 1;
    }

    std::cout
        << "phase,end_to_end_us,inference_us,checkpoint_or_kv_bytes\n"
        << "resident_create," << first.end_to_end_us << ','
        << first.result.metrics.execution_time_us << ','
        << first.result.metrics.cache_bytes << '\n'
        << "swap_out_create," << second.end_to_end_us << ','
        << second.result.metrics.execution_time_us << ','
        << second.result.metrics.cache_bytes << '\n'
        << "swap_in_continue," << restored.end_to_end_us << ','
        << restored.result.metrics.execution_time_us << ','
        << restored.result.metrics.cache_bytes << '\n'
        << "stats=" << stats_text << '\n';

    if (!release_context(&runtime, 100, first.result.context_id) ||
        !release_context(&runtime, 101, second.result.context_id)) {
        return 1;
    }
    runtime.shutdown();
    return 0;
}
