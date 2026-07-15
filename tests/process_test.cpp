#include <chrono>
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
    ar::AgentRuntime runtime(1);
    std::string error;
    CHECK(runtime.start(&error));

    ar::AgentTaskSpec task;
    task.id = 10;
    task.name = "process-agent";
    task.command = {"/bin/sh", "-c", "exit 0"};
    task.resources.timeout = std::chrono::seconds(2);
    CHECK(runtime.submit(task, &error));
    CHECK(runtime.wait_until_idle(std::chrono::seconds(5)));

    const auto snapshot = runtime.scheduler().snapshot(10);
    CHECK(snapshot.has_value());
    CHECK(snapshot->state == ar::AgentState::completed);

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
    runtime.stop();
    std::cout << "process tests passed\n";
    return 0;
}
