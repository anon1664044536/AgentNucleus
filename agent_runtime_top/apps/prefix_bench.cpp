#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include "top_runtime.h"

namespace ar = agent_runtime;
namespace art = agent_runtime_top;

namespace {

void usage() {
    std::cout
        << "usage: agent_top_prefix_bench [OPTIONS]\n"
        << "  --model PATH        GGUF model path\n"
        << "  --system-file PATH  UTF-8 system prompt file\n"
        << "  --rounds N          total cold+warm requests (default: 6)\n"
        << "  --threads N         CPU threads (default: 4)\n"
        << "  --kv-type TYPE      f16, q8_0, or q4_0\n";
}

template <typename T>
bool parse_unsigned(const std::string &text, T *value) {
    static_assert(std::is_unsigned_v<T>);
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), *value);
    return !text.empty() && result.ec == std::errc{} &&
           result.ptr == text.data() + text.size();
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

std::string json_escape(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size() + 32);
    for (const unsigned char character : value) {
        switch (character) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (character < 0x20U) {
                    const char hex[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped.push_back(hex[(character >> 4U) & 0x0FU]);
                    escaped.push_back(hex[character & 0x0FU]);
                } else {
                    escaped.push_back(static_cast<char>(character));
                }
        }
    }
    return escaped;
}

std::string default_system_prompt() {
    std::string prompt;
    for (int i = 0; i < 50; ++i) {
        prompt +=
            "You are an operating-system managed power-query Agent. "
            "Preserve schema constraints and return concise structured data. ";
    }
    return prompt;
}

}  // namespace

int main(int argc, char **argv) {
    art::TopRuntimeConfig config;
    config.model_alias = "bench";
    config.max_contexts = 1;
    config.max_tokens = 1;
    std::string system_file;
    std::uint32_t rounds = 6;

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
        } else if (option == "--system-file") {
            system_file = value;
        } else if (option == "--rounds") {
            if (!parse_unsigned(value, &rounds) || rounds < 2) {
                std::cerr << "rounds must be at least 2\n";
                return 2;
            }
        } else if (option == "--threads") {
            if (!parse_unsigned(value, &config.cpu_threads) ||
                config.cpu_threads == 0) {
                std::cerr << "invalid thread count\n";
                return 2;
            }
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

    std::string system_prompt;
    if (!system_file.empty()) {
        std::ifstream input(system_file, std::ios::binary);
        if (!input) {
            std::cerr << "cannot open system prompt file\n";
            return 1;
        }
        std::ostringstream contents;
        contents << input.rdbuf();
        system_prompt = contents.str();
    } else {
        system_prompt = default_system_prompt();
    }
    if (system_prompt.empty() || system_prompt.size() > LLM_MAX_PROMPT) {
        std::cerr << "system prompt must contain 1.." << LLM_MAX_PROMPT
                  << " bytes\n";
        return 2;
    }

    art::TopRuntime runtime(config);
    std::string error;
    if (!runtime.initialize(&error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::string payload =
        "{\"system_prompt\":\"" + json_escape(system_prompt) +
        "\",\"prompt\":\"Return OK.\",\"max_tokens\":1}";
    std::vector<std::uint64_t> latencies;
    latencies.reserve(rounds);

    std::cout
        << "round,cache_hit,reused_tokens,latency_us,cache_bytes\n";
    for (std::uint32_t round = 0; round < rounds; ++round) {
        const ar::AgentId agent_id = 10000 + round;
        ar::InvocationRequest request;
        request.agent_id = agent_id;
        request.kind = ar::InvocationKind::model;
        request.target = "bench";
        request.operation = "generate";
        request.payload = payload;
        request.resources.cpu_threads = config.cpu_threads;
        auto result = runtime.handle(request, {}, [] { return false; });
        if (result.status != ar::InvocationStatus::ok) {
            std::cerr << "benchmark request failed at round " << round
                      << ": " << result.message << '\n';
            return 1;
        }
        latencies.push_back(result.metrics.execution_time_us);
        std::cout << round << ','
                  << (result.metrics.cache_hit ? 1 : 0) << ','
                  << result.metrics.reused_tokens << ','
                  << result.metrics.execution_time_us << ','
                  << result.metrics.cache_bytes << '\n';

        ar::InvocationRequest release;
        release.agent_id = agent_id;
        release.kind = ar::InvocationKind::model;
        release.target = "bench";
        release.operation = "release_context";
        release.context_id = result.context_id;
        const auto released =
            runtime.handle(release, {}, [] { return false; });
        if (released.status != ar::InvocationStatus::ok) {
            std::cerr << "cannot release benchmark context\n";
            return 1;
        }
    }

    std::uint64_t warm_total = 0;
    for (std::size_t index = 1; index < latencies.size(); ++index) {
        warm_total += latencies[index];
    }
    const double warm_average =
        static_cast<double>(warm_total) /
        static_cast<double>(latencies.size() - 1);
    std::cout << std::fixed << std::setprecision(2)
              << "cold_latency_us=" << latencies.front() << '\n'
              << "warm_average_us=" << warm_average << '\n'
              << "cold_to_warm_speedup="
              << (warm_average > 0.0
                      ? static_cast<double>(latencies.front()) / warm_average
                      : 0.0)
              << '\n';
    runtime.shutdown();
    return 0;
}
