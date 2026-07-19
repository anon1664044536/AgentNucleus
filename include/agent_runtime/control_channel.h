#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "agent_runtime/control_protocol.h"
#include "agent_runtime/runtime.h"

namespace agent_runtime {

std::string default_control_socket_path();

class ControlClient {
public:
    explicit ControlClient(std::string socket_path = default_control_socket_path());

    bool request(const ControlRequest &request,
                 ControlResponse *response,
                 std::string *error = nullptr) const;

private:
    std::string socket_path_;
};

class AgentDaemon {
public:
    AgentDaemon(RuntimeConfig runtime_config,
                std::string socket_path = default_control_socket_path());
    ~AgentDaemon();

    AgentDaemon(const AgentDaemon &) = delete;
    AgentDaemon &operator=(const AgentDaemon &) = delete;

    bool start(std::string *error = nullptr);
    int serve(const std::function<bool()> &external_stop = {});
    void stop();
    const std::string &socket_path() const noexcept;

private:
    bool open_socket(std::string *error);
    void handle_client(int client_descriptor);
    ControlResponse dispatch(const ControlRequest &request);

    AgentRuntime runtime_;
    std::string socket_path_;
    int listen_descriptor_{-1};
    bool owns_socket_{false};
    std::atomic_bool running_{false};
};

}  // namespace agent_runtime
