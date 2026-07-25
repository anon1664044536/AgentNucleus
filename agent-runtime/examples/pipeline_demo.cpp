#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "agent_runtime/runtime.h"

namespace ar = agent_runtime;

int main() {
    ar::AgentRuntime runtime(2);
    std::mutex output_mutex;
    std::unordered_map<ar::AgentId, std::string> outputs;

    runtime.register_handler("pipeline", [&](const ar::AgentSnapshot &agent) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::lock_guard lock(output_mutex);
        outputs[agent.spec.id] = "result-from-" + agent.spec.name;
        std::cout << "completed " << agent.spec.name << '\n';
        return ar::AgentExecutionResult{};
    });

    std::string error;
    if (!runtime.start(&error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::vector<ar::AgentTaskSpec> tasks;
    tasks.push_back({.id = 100, .name = "intent-parser", .kind = "pipeline"});
    tasks.push_back({.id = 101,
                     .name = "schema-agent",
                     .kind = "pipeline",
                     .dependencies = {100}});
    tasks.push_back({.id = 102,
                     .name = "policy-agent",
                     .kind = "pipeline",
                     .dependencies = {100}});
    tasks.push_back({.id = 103,
                     .name = "query-generator",
                     .kind = "pipeline",
                     .dependencies = {101, 102}});

    if (!runtime.submit_batch(tasks, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (!runtime.wait_until_idle(std::chrono::seconds(5))) {
        std::cerr << "pipeline timeout\n";
        return 1;
    }
    runtime.stop();

    for (const auto &agent : runtime.scheduler().snapshots()) {
        std::cout << agent.spec.id << ' ' << ar::to_string(agent.state) << '\n';
        if (agent.state != ar::AgentState::completed) return 1;
    }
    return 0;
}

