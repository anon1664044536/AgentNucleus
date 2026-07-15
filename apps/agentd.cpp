#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "agent_runtime/resource_monitor.h"
#include "agent_runtime/runtime.h"

namespace ar = agent_runtime;

namespace {

int run_demo(bool enable_cgroups) {
    ar::RuntimeConfig config;
    config.worker_count = 2;
    config.enable_cgroups = enable_cgroups;
    if (const char *root = std::getenv("AGENT_RUNTIME_CGROUP_ROOT")) {
        config.cgroup_root = root;
    }
    ar::AgentRuntime runtime(config);

    runtime.register_handler("demo", [](const ar::AgentSnapshot &agent) {
        std::cout << "agent " << agent.spec.id << " (" << agent.spec.name
                  << ") is running\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        return ar::AgentExecutionResult{};
    });

    std::string error;
    if (!runtime.start(&error)) {
        std::cerr << "failed to start runtime: " << error << '\n';
        return 1;
    }

    std::vector<ar::AgentTaskSpec> tasks(4);
    tasks[0].id = 1;
    tasks[0].name = "parse-request";
    tasks[0].kind = "demo";

    tasks[1].id = 2;
    tasks[1].name = "lookup-schema";
    tasks[1].kind = "demo";
    tasks[1].dependencies = {1};

    tasks[2].id = 3;
    tasks[2].name = "retrieve-policy";
    tasks[2].kind = "demo";
    tasks[2].dependencies = {1};

    tasks[3].id = 4;
    tasks[3].name = "generate-query";
    tasks[3].kind = "demo";
    tasks[3].dependencies = {2, 3};

    if (enable_cgroups) {
        for (auto &task : tasks) {
            task.command = {"/bin/sh", "-c", "sleep 0.08"};
            task.resources.cpu_threads = 1;
            task.resources.memory_bytes = 128ULL * 1024ULL * 1024ULL;
            task.resources.timeout = std::chrono::seconds(2);
        }
    }

    if (!runtime.submit_batch(tasks, &error)) {
        std::cerr << "failed to submit DAG: " << error << '\n';
        return 1;
    }
    if (!runtime.wait_until_idle(std::chrono::seconds(10))) {
        std::cerr << "runtime did not become idle\n";
        return 1;
    }

    std::cout << "\nID  PID       STATE                NAME\n";
    for (const auto &agent : runtime.scheduler().snapshots()) {
        std::cout << std::left << std::setw(4) << agent.spec.id << std::setw(10)
                  << agent.process_id << std::setw(21)
                  << ar::to_string(agent.state) << agent.spec.name << '\n';
    }
    runtime.stop();
    return 0;
}

int show_memory() {
    ar::ResourceMonitor monitor;
    std::string error;
    const auto memory = monitor.sample(&error);
    if (!memory.has_value()) {
        std::cerr << error << '\n';
        return 1;
    }
    constexpr double mib = 1024.0 * 1024.0;
    std::cout << "available_cpu_threads=" << memory->available_cpu_threads << '\n'
              << "total_mib=" << memory->total_bytes / mib << '\n'
              << "available_mib=" << memory->available_bytes / mib << '\n'
              << "cgroup_current_mib=" << memory->cgroup_current_bytes / mib << '\n'
              << "cgroup_limit_mib=" << memory->cgroup_limit_bytes / mib << '\n'
              << "memory_pressure_avg10=" << memory->pressure_avg10 << '\n';
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string command = argc > 1 ? argv[1] : "--demo";
    if (command == "--demo") return run_demo(false);
    if (command == "--demo-cgroup") return run_demo(true);
    if (command == "--memory") return show_memory();
    std::cout << "usage: agentd [--demo|--demo-cgroup|--memory]\n";
    return command == "--help" ? 0 : 2;
}
