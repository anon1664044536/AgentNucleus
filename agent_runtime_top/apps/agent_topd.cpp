#include <charconv>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "agent_runtime/invocation_channel.h"
#include "top_runtime.h"

namespace ar = agent_runtime;
namespace art = agent_runtime_top;

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) {
    stop_requested = 1;
}

void usage() {
    std::cout
        << "usage: agent_topd [OPTIONS]\n"
        << "  --model PATH          GGUF model path\n"
        << "  --alias NAME          model target alias (default: default)\n"
        << "  --socket PATH         invocation socket path\n"
        << "  --ctx-tokens N        context token capacity\n"
        << "  --batch-tokens N      logical llama.cpp batch size\n"
        << "  --threads N           CPU inference threads\n"
        << "  --max-contexts N      private Agent context slots\n"
        << "  --max-swapped-contexts N  additional disk-backed contexts\n"
        << "  --swap-dir PATH       context checkpoint directory\n"
        << "  --max-tokens N        default generated token limit\n"
        << "  --temperature N       default sampling temperature\n"
        << "  --top-p N             default nucleus sampling threshold\n"
        << "  --gpu-layers N        layers offloaded to GPU (0 for CPU)\n"
        << "  --kv-type TYPE        f16, q8_0, or q4_0\n"
        << "  --mlock               ask the OS to keep model pages resident\n"
        << "  --no-mmap             disable model file memory mapping\n"
        << "  --no-chat-template    tokenize prompts without model template\n"
        << "  --enable-shell-tool   register the opt-in shell tool\n";
}

template <typename T>
bool parse_number(const std::string &text, T *value) {
    static_assert(std::is_integral_v<T>);
    if (text.empty()) return false;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), *value);
    return parsed.ec == std::errc{} &&
           parsed.ptr == text.data() + text.size();
}

bool parse_float(const std::string &text, float *value) {
    if (text.empty()) return false;
    errno = 0;
    char *end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (errno == ERANGE || end != text.c_str() + text.size() ||
        !std::isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
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

}  // namespace

int main(int argc, char **argv) {
    art::TopRuntimeConfig config;
    std::string socket_path = ar::default_invocation_socket_path();

    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help" || option == "-h") {
            usage();
            return 0;
        }
        if (option == "--enable-shell-tool") {
            config.enable_shell_tool = true;
            continue;
        }
        if (option == "--mlock") {
            config.use_mlock = true;
            continue;
        }
        if (option == "--no-mmap") {
            config.use_mmap = false;
            continue;
        }
        if (option == "--no-chat-template") {
            config.use_chat_template = false;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << option << '\n';
            return 2;
        }
        const std::string value = argv[++index];
        if (option == "--model") {
            config.model_path = value;
        } else if (option == "--alias") {
            config.model_alias = value;
        } else if (option == "--socket") {
            socket_path = value;
        } else if (option == "--ctx-tokens") {
            if (!parse_number(value, &config.context_tokens) ||
                config.context_tokens == 0) {
                std::cerr << "invalid context token count\n";
                return 2;
            }
        } else if (option == "--batch-tokens") {
            if (!parse_number(value, &config.batch_tokens) ||
                config.batch_tokens == 0) {
                std::cerr << "invalid batch token count\n";
                return 2;
            }
        } else if (option == "--threads") {
            if (!parse_number(value, &config.cpu_threads) ||
                config.cpu_threads == 0) {
                std::cerr << "invalid thread count\n";
                return 2;
            }
        } else if (option == "--max-contexts") {
            if (!parse_number(value, &config.max_contexts) ||
                config.max_contexts == 0) {
                std::cerr << "invalid context count\n";
                return 2;
            }
        } else if (option == "--max-swapped-contexts") {
            if (!parse_number(value, &config.max_swapped_contexts) ||
                config.max_swapped_contexts == 0) {
                std::cerr << "invalid swapped context count\n";
                return 2;
            }
            config.enable_context_swap = true;
        } else if (option == "--swap-dir") {
            if (value.empty()) {
                std::cerr << "invalid swap directory\n";
                return 2;
            }
            config.swap_directory = value;
            config.enable_context_swap = true;
            if (config.max_swapped_contexts == 0) {
                config.max_swapped_contexts = 32;
            }
        } else if (option == "--max-tokens") {
            if (!parse_number(value, &config.max_tokens) ||
                config.max_tokens == 0) {
                std::cerr << "invalid generated token limit\n";
                return 2;
            }
        } else if (option == "--temperature") {
            if (!parse_float(value, &config.temperature) ||
                config.temperature <= 0.0F ||
                config.temperature > 2.0F) {
                std::cerr << "invalid sampling temperature\n";
                return 2;
            }
        } else if (option == "--top-p") {
            if (!parse_float(value, &config.top_p) ||
                config.top_p <= 0.0F || config.top_p > 1.0F) {
                std::cerr << "invalid top-p threshold\n";
                return 2;
            }
        } else if (option == "--gpu-layers") {
            if (!parse_number(value, &config.gpu_layers) ||
                config.gpu_layers < 0) {
                std::cerr << "invalid GPU layer count\n";
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

    art::TopRuntime runtime(std::move(config));
    std::string error;
    if (!runtime.initialize(&error)) {
        std::cerr << "failed to initialize top runtime: " << error << '\n';
        return 1;
    }

    ar::InvocationServer server(
        [&](const ar::InvocationRequest &request,
            const std::vector<ar::InvocationMappedInput> &inputs,
            const std::function<bool()> &cancel_requested) {
            return runtime.handle(request, inputs, cancel_requested);
        },
        socket_path);
    if (!server.start(&error)) {
        std::cerr << "failed to start invocation server: " << error << '\n';
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::cout << "agent_topd listening on " << server.socket_path() << '\n';
    const int result =
        server.serve([] { return stop_requested != 0; });
    runtime.shutdown();
    return result;
}
