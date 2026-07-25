#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "agent_runtime/control_channel.h"

namespace ar = agent_runtime;

namespace {

bool exchange(const ar::ControlClient &client,
              const ar::ControlRequest &request,
              ar::ControlResponse *response) {
    std::string error;
    if (!client.request(request, response, &error)) {
        std::cerr << error << '\n';
        return false;
    }
    if (!response->success) {
        std::cerr << response->message << '\n';
        return false;
    }
    return true;
}

bool wait_for_state(const ar::ControlClient &client,
                    ar::AgentId id,
                    ar::AgentState expected,
                    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    ar::ControlRequest request;
    request.operation = ar::ControlOperation::status;
    request.target_id = id;
    do {
        ar::ControlResponse response;
        if (!exchange(client, request, &response) || response.agents.empty()) {
            return false;
        }
        if (response.agents[0].state == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

}  // namespace

int main() {
    const std::string socket_path =
        "/tmp/agentnucleus-test-" + std::to_string(getpid()) + ".sock";
    ar::RuntimeConfig config;
    config.worker_count = 2;
    ar::AgentDaemon daemon(config, socket_path);
    std::string error;
    if (!daemon.start(&error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::atomic_bool stop{false};
    std::thread server([&] { daemon.serve([&] { return stop.load(); }); });
    ar::ControlClient client(socket_path);
    bool passed = true;

    ar::AgentDaemon duplicate(config, socket_path);
    std::string duplicate_error;
    passed = !duplicate.start(&duplicate_error) &&
             duplicate_error == "control socket is already in use" && passed;

    ar::ControlRequest ping;
    ping.operation = ar::ControlOperation::ping;
    ar::ControlResponse response;
    passed = exchange(client, ping, &response) && passed;

    ar::ControlRequest submit;
    submit.operation = ar::ControlOperation::submit;
    submit.task.id = 100;
    submit.task.name = "root";
    submit.task.kind = "command";
    submit.task.command = {
        "/bin/sh", "-c", "printf 'root-output'; sleep 0.2"};
    submit.task.resources.timeout = std::chrono::seconds(2);
    passed = exchange(client, submit, &response) && passed;

    ar::ControlRequest spawn;
    spawn.operation = ar::ControlOperation::spawn;
    spawn.target_id = 100;
    spawn.task.id = 101;
    spawn.task.name = "dynamic-child";
    spawn.task.kind = "command";
    spawn.task.dependencies = {100};
    spawn.task.command = {"/bin/sh", "-c", "exit 0"};
    passed = exchange(client, spawn, &response) && passed;
    passed = wait_for_state(client,
                            101,
                            ar::AgentState::completed,
                            std::chrono::seconds(3)) &&
             passed;

    ar::ControlResult root_result;
    passed = client.fetch_result(100, &root_result, &error) && passed;
    if (root_result.region.valid()) {
        const std::string result_text(
            static_cast<const char *>(root_result.region.data()) +
                root_result.reference.offset,
            root_result.reference.length);
        passed = result_text == "root-output" && !root_result.truncated() && passed;
    } else {
        passed = false;
    }
    ar::ControlRequest release_result;
    release_result.operation = ar::ControlOperation::release_result;
    release_result.target_id = 100;
    passed = exchange(client, release_result, &response) && passed;
    ar::ControlResult released_result;
    passed = !client.fetch_result(100, &released_result, &error) && passed;
    if (root_result.region.valid()) {
        const std::string retained_after_release(
            static_cast<const char *>(root_result.region.data()),
            root_result.reference.length);
        passed = retained_after_release == "root-output" && passed;
    }

    ar::ControlRequest list;
    list.operation = ar::ControlOperation::list;
    passed = exchange(client, list, &response) && passed;
    passed = response.agents.size() == 2 && passed;

    submit.task.id = 102;
    submit.task.name = "cancel-me";
    submit.task.command = {"/bin/sh", "-c", "sleep 5"};
    submit.task.resources.timeout = std::chrono::seconds(10);
    passed = exchange(client, submit, &response) && passed;
    passed = wait_for_state(client,
                            102,
                            ar::AgentState::running,
                            std::chrono::seconds(2)) &&
             passed;
    ar::ControlRequest cancel;
    cancel.operation = ar::ControlOperation::cancel;
    cancel.target_id = 102;
    passed = exchange(client, cancel, &response) && passed;
    passed = wait_for_state(client,
                            102,
                            ar::AgentState::cancelled,
                            std::chrono::seconds(2)) &&
             passed;

    ar::ControlRequest shutdown;
    shutdown.operation = ar::ControlOperation::shutdown;
    passed = exchange(client, shutdown, &response) && passed;
    stop.store(true);
    server.join();

    if (!passed) return 1;
    std::cout << "control plane tests passed\n";
    return 0;
}
