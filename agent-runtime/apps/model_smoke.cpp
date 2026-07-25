#include <algorithm>
#include <iostream>
#include <string>
#include <thread>

#include "agent_runtime/model_engine.h"

namespace ar = agent_runtime;

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: agent_model_smoke /path/to/model.gguf\n";
        return 2;
    }

    ar::ModelEngineConfig config;
    config.model_path = argv[1];
    config.cpu_threads =
        std::max(1U, std::thread::hardware_concurrency() / 2U);
    config.max_contexts = 1;

    ar::ModelEngine engine(config);
    std::string error;
    if (!engine.initialize(&error)) {
        std::cerr << "model initialization failed: " << error << '\n';
        return 1;
    }
    const auto context = engine.acquire(1, &error);
    if (!context.has_value()) {
        std::cerr << "context admission failed: " << error << '\n';
        return 1;
    }
    std::cout << "CPU model loaded; context_id=" << *context
              << " active_contexts=" << engine.active_contexts() << '\n';
    if (!engine.release(*context, &error)) {
        std::cerr << "context release failed: " << error << '\n';
        return 1;
    }
    return 0;
}
