#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agent_runtime/shared_memory.h"
#include "agent_runtime/types.h"

namespace agent_runtime {

constexpr std::uint32_t kInvocationProtocolMagic = 0x41524956U;
constexpr std::uint16_t kInvocationProtocolVersion = 1;
constexpr std::size_t kMaxInvocationMessageSize = 1024U * 1024U;
constexpr std::size_t kMaxInvocationInputs = 64;

enum class InvocationStatus : std::uint8_t {
    ok = 0,
    rejected = 1,
    failed = 2,
    cancelled = 3,
    busy = 4,
};

struct InvocationInput {
    AgentId producer_id{kInvalidAgentId};
    SharedBufferRef reference;
};

using InvocationMetrics = AgentPerformanceMetrics;

struct InvocationRequest {
    AgentId agent_id{kInvalidAgentId};
    AgentId parent_id{kInvalidAgentId};
    InvocationKind kind{InvocationKind::model};
    std::string target;
    std::string operation;
    std::string payload;
    ContextId context_id{kInvalidContextId};
    int priority{0};
    ResourceRequest resources;
    std::vector<InvocationInput> inputs;
};

struct InvocationResponse {
    AgentId agent_id{kInvalidAgentId};
    std::int64_t process_id{-1};
    InvocationStatus status{InvocationStatus::failed};
    std::string message;
    ContextId context_id{kInvalidContextId};
    InvocationMetrics metrics;
    bool output_available{false};
    SharedBufferRef output;
};

bool encode_invocation_request(const InvocationRequest &request,
                               std::vector<std::uint8_t> *output,
                               std::string *error = nullptr);
bool decode_invocation_request(const std::uint8_t *data,
                               std::size_t size,
                               InvocationRequest *request,
                               std::string *error = nullptr);
bool encode_invocation_response(const InvocationResponse &response,
                                std::vector<std::uint8_t> *output,
                                std::string *error = nullptr);
bool decode_invocation_response(const std::uint8_t *data,
                                std::size_t size,
                                InvocationResponse *response,
                                std::string *error = nullptr);

}  // namespace agent_runtime
