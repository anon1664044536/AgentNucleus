#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "agent_runtime/control_protocol.h"

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
    ar::ControlRequest source;
    source.operation = ar::ControlOperation::spawn;
    source.target_id = 41;
    source.task.id = 42;
    source.task.parent_id = 41;
    source.task.name = "dynamic child";
    source.task.kind = "command";
    source.task.priority = -3;
    source.task.dependencies = {40, 41};
    source.task.resources.cpu_threads = 2;
    source.task.resources.memory_bytes = 256ULL * 1024ULL * 1024ULL;
    source.task.resources.timeout = std::chrono::milliseconds(1234);
    source.task.retry_limit = 2;
    source.task.command = {"/bin/sh", "-c", "printf 'hello world'"};

    std::vector<std::uint8_t> encoded;
    std::string error;
    CHECK(ar::encode_control_request(source, &encoded, &error));
    ar::ControlRequest decoded;
    CHECK(ar::decode_control_request(
        encoded.data(), encoded.size(), &decoded, &error));
    CHECK(decoded.operation == source.operation);
    CHECK(decoded.target_id == source.target_id);
    CHECK(decoded.task.id == source.task.id);
    CHECK(decoded.task.name == source.task.name);
    CHECK(decoded.task.priority == source.task.priority);
    CHECK(decoded.task.dependencies == source.task.dependencies);
    CHECK(decoded.task.command == source.task.command);
    CHECK(decoded.task.resources.memory_bytes ==
          source.task.resources.memory_bytes);
    CHECK(decoded.task.resources.timeout == source.task.resources.timeout);
    CHECK(decoded.task.retry_limit == source.task.retry_limit);

    ar::ControlResponse response;
    response.success = true;
    response.message = "ok";
    response.agents.push_back({.id = 42,
                               .parent_id = 41,
                               .state = ar::AgentState::running,
                               .process_id = -1,
                               .name = "dynamic child",
                               .kind = "command"});
    response.result_available = true;
    response.result = {.region_id = 99,
                       .region_size = 4096,
                       .offset = 32,
                       .length = 128,
                       .data_type = static_cast<std::uint32_t>(
                           ar::SharedDataType::text_utf8),
                       .flags = ar::shared_buffer_immutable,
                       .version = 1};
    CHECK(ar::encode_control_response(response, &encoded, &error));
    ar::ControlResponse decoded_response;
    CHECK(ar::decode_control_response(
        encoded.data(), encoded.size(), &decoded_response, &error));
    CHECK(decoded_response.success);
    CHECK(decoded_response.agents.size() == 1);
    CHECK(decoded_response.agents[0].process_id == -1);
    CHECK(decoded_response.agents[0].state == ar::AgentState::running);
    CHECK(decoded_response.result_available);
    CHECK(decoded_response.result.region_id == 99);
    CHECK(decoded_response.result.offset == 32);
    CHECK(decoded_response.result.length == 128);

    encoded[0] = 0;
    CHECK(!ar::decode_control_response(
        encoded.data(), encoded.size(), &decoded_response, &error));

    source.task.resources.timeout = std::chrono::milliseconds(-1);
    CHECK(!ar::encode_control_request(source, &encoded, &error));
    CHECK(!error.empty());
    std::cout << "control protocol tests passed\n";
    return 0;
}
