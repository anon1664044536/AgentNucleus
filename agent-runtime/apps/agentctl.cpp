#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "agent_runtime/control_channel.h"

namespace ar = agent_runtime;

namespace {

void usage() {
    std::cout
        << "usage: agentctl [--socket PATH] COMMAND\n"
        << "commands:\n"
        << "  ping\n"
        << "  submit ID NAME [OPTIONS] -- PROGRAM [ARGS...]\n"
        << "  spawn PARENT_ID ID NAME [OPTIONS] -- PROGRAM [ARGS...]\n"
        << "  invoke ID NAME model|tool TARGET OPERATION [OPTIONS] -- PAYLOAD\n"
        << "  spawn-invoke PARENT_ID ID NAME model|tool TARGET OPERATION\n"
        << "               [OPTIONS] -- PAYLOAD\n"
        << "  status ID\n"
        << "  list\n"
        << "  wait ID [TIMEOUT_MS]\n"
        << "  result ID\n"
        << "  release-result ID\n"
        << "  cancel ID\n"
        << "  shutdown\n"
        << "task options:\n"
        << "  --depends ID,ID  --priority N  --cpu N\n"
        << "  --memory-mib N   --timeout-ms N --retries N\n"
        << "  --context ID     (invocation tasks only)\n";
}

template <typename T>
bool parse_unsigned(const std::string &text, T *value) {
    static_assert(std::is_unsigned_v<T>);
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_int(const std::string &text, int *value) {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_dependencies(const std::string &text, std::vector<ar::AgentId> *result) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t comma = text.find(',', begin);
        const std::string item = text.substr(
            begin, comma == std::string::npos ? std::string::npos : comma - begin);
        ar::AgentId id = 0;
        if (!parse_unsigned(item, &id) || id == ar::kInvalidAgentId) return false;
        result->push_back(id);
        if (comma == std::string::npos) return true;
        begin = comma + 1;
    }
    return text.empty();
}

bool parse_task_options(int argc,
                        char **argv,
                        int *index,
                        ar::AgentTaskSpec *task,
                        bool parse_command,
                        std::string *error) {
    while (*index < argc && std::string(argv[*index]) != "--") {
        const std::string option = argv[(*index)++];
        if (*index >= argc) {
            *error = "missing value for " + option;
            return false;
        }
        const std::string value = argv[(*index)++];
        if (option == "--depends") {
            if (!parse_dependencies(value, &task->dependencies)) {
                *error = "invalid dependency list";
                return false;
            }
        } else if (option == "--priority") {
            if (!parse_int(value, &task->priority)) {
                *error = "invalid priority";
                return false;
            }
        } else if (option == "--cpu") {
            if (!parse_unsigned(value, &task->resources.cpu_threads) ||
                task->resources.cpu_threads == 0) {
                *error = "invalid CPU thread count";
                return false;
            }
        } else if (option == "--memory-mib") {
            std::uint64_t mib = 0;
            if (!parse_unsigned(value, &mib) ||
                mib > std::numeric_limits<std::uint64_t>::max() / (1024ULL * 1024ULL)) {
                *error = "invalid memory limit";
                return false;
            }
            task->resources.memory_bytes = mib * 1024ULL * 1024ULL;
        } else if (option == "--timeout-ms") {
            std::uint64_t milliseconds = 0;
            if (!parse_unsigned(value, &milliseconds) ||
                milliseconds > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max())) {
                *error = "invalid timeout";
                return false;
            }
            task->resources.timeout = std::chrono::milliseconds(milliseconds);
        } else if (option == "--retries") {
            if (!parse_unsigned(value, &task->retry_limit)) {
                *error = "invalid retry count";
                return false;
            }
        } else if (option == "--context") {
            if (!task->invocation.has_value() ||
                !parse_unsigned(value, &task->invocation->context_id)) {
                *error = "invalid invocation context id";
                return false;
            }
        } else {
            *error = "unknown task option: " + option;
            return false;
        }
    }
    if (*index >= argc || std::string(argv[*index]) != "--") {
        *error = "task command must follow --";
        return false;
    }
    ++(*index);
    if (parse_command) {
        while (*index < argc) task->command.emplace_back(argv[(*index)++]);
        if (task->command.empty()) {
            *error = "task command is empty";
            return false;
        }
        task->kind = "command";
    } else {
        std::string payload;
        while (*index < argc) {
            if (!payload.empty()) payload.push_back(' ');
            payload.append(argv[(*index)++]);
        }
        task->invocation->payload = std::move(payload);
        task->kind = "invocation";
    }
    return true;
}

void print_agents(const std::vector<ar::ControlAgentInfo> &agents) {
    std::cout << std::left << std::setw(8) << "ID" << std::setw(8) << "PARENT"
              << std::setw(10) << "PID" << std::setw(22) << "STATE"
              << std::setw(14) << "KIND" << std::setw(10) << "CONTEXT"
              << "NAME\n";
    for (const auto &agent : agents) {
        std::cout << std::left << std::setw(8) << agent.id << std::setw(8)
                  << agent.parent_id << std::setw(10) << agent.process_id
                  << std::setw(22) << ar::to_string(agent.state)
                  << std::setw(14) << agent.kind << std::setw(10)
                  << agent.context_id << agent.name;
        if (!agent.error.empty()) std::cout << "  error=" << agent.error;
        if (agent.metrics.execution_time_us != 0 ||
            agent.metrics.input_tokens != 0 ||
            agent.metrics.output_tokens != 0) {
            std::cout << "  exec_us=" << agent.metrics.execution_time_us
                      << " input_tokens=" << agent.metrics.input_tokens
                      << " output_tokens=" << agent.metrics.output_tokens
                      << " cache_hit="
                      << (agent.metrics.cache_hit ? "yes" : "no")
                      << " reused_tokens=" << agent.metrics.reused_tokens;
        }
        std::cout << '\n';
    }
}

bool send_request(const ar::ControlClient &client,
                  const ar::ControlRequest &request,
                  ar::ControlResponse *response) {
    std::string error;
    if (!client.request(request, response, &error)) {
        std::cerr << "control request failed: " << error << '\n';
        return false;
    }
    if (!response->success) {
        std::cerr << "agentd rejected request: " << response->message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    int index = 1;
    std::string socket_path = ar::default_control_socket_path();
    if (index < argc && std::string(argv[index]) == "--socket") {
        if (++index >= argc) {
            usage();
            return 2;
        }
        socket_path = argv[index++];
    }
    if (index >= argc) {
        usage();
        return 2;
    }

    const std::string command = argv[index++];
    ar::ControlRequest request;
    std::string parse_error;

    if (command == "ping") {
        request.operation = ar::ControlOperation::ping;
    } else if (command == "list") {
        request.operation = ar::ControlOperation::list;
    } else if (command == "shutdown") {
        request.operation = ar::ControlOperation::shutdown;
    } else if (command == "status" || command == "cancel" || command == "wait" ||
               command == "result" || command == "release-result") {
        if (index >= argc || !parse_unsigned(std::string(argv[index++]), &request.target_id)) {
            std::cerr << "invalid agent id\n";
            return 2;
        }
        if (command == "cancel") {
            request.operation = ar::ControlOperation::cancel;
        } else if (command == "result") {
            request.operation = ar::ControlOperation::result;
        } else if (command == "release-result") {
            request.operation = ar::ControlOperation::release_result;
        } else {
            request.operation = ar::ControlOperation::status;
        }
    } else if (command == "submit" || command == "spawn" ||
               command == "invoke" || command == "spawn-invoke") {
        const bool spawn =
            command == "spawn" || command == "spawn-invoke";
        const bool invocation =
            command == "invoke" || command == "spawn-invoke";
        request.operation = spawn ? ar::ControlOperation::spawn
                                  : ar::ControlOperation::submit;
        if (spawn) {
            if (index >= argc ||
                !parse_unsigned(std::string(argv[index++]), &request.target_id)) {
                std::cerr << "invalid parent agent id\n";
                return 2;
            }
        }
        if (index + 1 >= argc ||
            !parse_unsigned(std::string(argv[index++]), &request.task.id) ||
            request.task.id == ar::kInvalidAgentId) {
            std::cerr << "invalid new agent id\n";
            return 2;
        }
        request.task.name = argv[index++];
        if (invocation) {
            if (index + 2 >= argc) {
                std::cerr << "invocation kind, target, and operation are required\n";
                return 2;
            }
            const std::string invocation_kind = argv[index++];
            ar::InvocationSpec spec;
            if (invocation_kind == "model") {
                spec.kind = ar::InvocationKind::model;
            } else if (invocation_kind == "tool") {
                spec.kind = ar::InvocationKind::tool;
            } else {
                std::cerr << "invocation kind must be model or tool\n";
                return 2;
            }
            spec.target = argv[index++];
            spec.operation = argv[index++];
            request.task.invocation = std::move(spec);
        }
        if (!parse_task_options(
                argc,
                argv,
                &index,
                &request.task,
                !invocation,
                &parse_error)) {
            std::cerr << parse_error << '\n';
            return 2;
        }
    } else if (command == "--help" || command == "help") {
        usage();
        return 0;
    } else {
        usage();
        return 2;
    }

    ar::ControlClient client(socket_path);
    if (command == "result") {
        ar::ControlResult result;
        std::string error;
        if (!client.fetch_result(request.target_id, &result, &error)) {
            std::cerr << "failed to fetch agent result: " << error << '\n';
            return 1;
        }
        const auto *bytes = static_cast<const char *>(result.region.data()) +
                            static_cast<std::size_t>(result.reference.offset);
        std::size_t remaining =
            static_cast<std::size_t>(result.reference.length);
        while (remaining > 0) {
            const std::size_t count = std::min<std::size_t>(remaining, 1024U * 1024U);
            std::cout.write(bytes, static_cast<std::streamsize>(count));
            if (!std::cout) return 1;
            bytes += count;
            remaining -= count;
        }
        if (result.truncated()) {
            std::cerr << "\nwarning: agent output was truncated\n";
        }
        return std::cout ? 0 : 1;
    }
    if (command == "wait") {
        std::uint64_t timeout_ms = 30000;
        if (index < argc && !parse_unsigned(std::string(argv[index]), &timeout_ms)) {
            std::cerr << "invalid wait timeout\n";
            return 2;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        for (;;) {
            ar::ControlResponse response;
            if (!send_request(client, request, &response)) return 1;
            if (!response.agents.empty() && ar::is_terminal(response.agents[0].state)) {
                print_agents(response.agents);
                return response.agents[0].state == ar::AgentState::completed ? 0 : 1;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "wait timed out\n";
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    ar::ControlResponse response;
    if (!send_request(client, request, &response)) return 1;
    if (!response.message.empty()) std::cout << response.message << '\n';
    if (!response.agents.empty()) print_agents(response.agents);
    return 0;
}
