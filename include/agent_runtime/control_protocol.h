#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agent_runtime/shared_memory.h"
#include "agent_runtime/types.h"

namespace agent_runtime {

constexpr std::uint32_t kControlProtocolMagic = 0x41525443U;
constexpr std::uint16_t kControlProtocolVersion = 2;
constexpr std::size_t kMaxControlMessageSize = 1024U * 1024U;

enum class ControlOperation : std::uint16_t {
    ping = 1,
    submit = 2,
    spawn = 3,
    status = 4,
    list = 5,
    cancel = 6,
    shutdown = 7,
    result = 8,
    release_result = 9,
};

struct ControlRequest {
    ControlOperation operation{ControlOperation::ping};
    AgentId target_id{kInvalidAgentId};
    AgentTaskSpec task;
};

struct ControlAgentInfo {
    AgentId id{kInvalidAgentId};
    AgentId parent_id{kInvalidAgentId};
    AgentState state{AgentState::created};
    std::int64_t process_id{-1};
    std::uint32_t unresolved_dependencies{0};
    std::uint32_t retry_count{0};
    ContextId context_id{kInvalidContextId};
    std::string name;
    std::string kind;
    std::string error;
};

struct ControlResponse {
    bool success{false};
    std::string message;
    std::vector<ControlAgentInfo> agents;
    bool result_available{false};
    SharedBufferRef result;
};

ControlAgentInfo make_control_info(const AgentSnapshot &snapshot);

bool encode_control_request(const ControlRequest &request,
                            std::vector<std::uint8_t> *output,
                            std::string *error = nullptr);
bool decode_control_request(const std::uint8_t *data,
                            std::size_t size,
                            ControlRequest *request,
                            std::string *error = nullptr);
bool encode_control_response(const ControlResponse &response,
                             std::vector<std::uint8_t> *output,
                             std::string *error = nullptr);
bool decode_control_response(const std::uint8_t *data,
                             std::size_t size,
                             ControlResponse *response,
                             std::string *error = nullptr);

}  // namespace agent_runtime
