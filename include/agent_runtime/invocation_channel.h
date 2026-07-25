#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "agent_runtime/invocation_protocol.h"

namespace agent_runtime {

std::string default_invocation_socket_path();

struct InvocationAttachment {
    AgentId producer_id{kInvalidAgentId};
    SharedBufferRef reference;
    int descriptor{-1};
};

struct InvocationCallResult {
    InvocationResponse response;
    SharedMemoryRegion output;
};

class InvocationClient {
public:
    explicit InvocationClient(
        std::string socket_path = default_invocation_socket_path());

    bool invoke(const InvocationRequest &request,
                const std::vector<InvocationAttachment> &attachments,
                InvocationCallResult *result,
                std::chrono::milliseconds timeout,
                const std::atomic_bool *cancel_requested = nullptr,
                std::string *error = nullptr) const;

private:
    std::string socket_path_;
};

struct InvocationMappedInput {
    AgentId producer_id{kInvalidAgentId};
    SharedBufferRef reference;
    SharedMemoryRegion region;
};

struct InvocationHandlerResult {
    InvocationStatus status{InvocationStatus::ok};
    std::string message;
    std::int64_t process_id{-1};
    ContextId context_id{kInvalidContextId};
    InvocationMetrics metrics;
    SharedMemoryRegion output;
    std::size_t output_length{0};
    SharedDataType output_type{SharedDataType::binary};
    std::uint32_t output_flags{shared_buffer_none};
};

using InvocationHandler = std::function<InvocationHandlerResult(
    const InvocationRequest &,
    const std::vector<InvocationMappedInput> &,
    const std::function<bool()> &cancel_requested)>;

class InvocationServer {
public:
    InvocationServer(
        InvocationHandler handler,
        std::string socket_path = default_invocation_socket_path());
    ~InvocationServer();

    InvocationServer(const InvocationServer &) = delete;
    InvocationServer &operator=(const InvocationServer &) = delete;

    bool start(std::string *error = nullptr);
    int serve(const std::function<bool()> &external_stop = {});
    void stop();
    const std::string &socket_path() const noexcept;

private:
    bool open_socket(std::string *error);
    void handle_client(int client_descriptor);

    InvocationHandler handler_;
    std::string socket_path_;
    int listen_descriptor_{-1};
    bool owns_socket_{false};
    std::atomic_bool running_{false};
    std::size_t active_clients_{0};
    std::mutex clients_mutex_;
    std::condition_variable clients_cv_;
};

}  // namespace agent_runtime
