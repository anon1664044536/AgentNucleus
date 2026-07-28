#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "agent_runtime/control_channel.h"

namespace ar = agent_runtime;

namespace {

constexpr std::size_t kMaxSchemaBytes = 120U * 1024U;

struct Options {
    std::filesystem::path schema_path{"../pms_schema.sql"};
    std::string question{
        "查询2025年投运且电压等级为110kV的主变压器，按运维单位统计数量。"};
    std::string model_alias{"pms"};
    std::string socket_path{ar::default_control_socket_path()};
    std::uint64_t timeout_ms{180000};
    std::uint32_t cpu_threads{4};
    std::uint64_t memory_mib{256};
    ar::AgentId id_base{0};
    bool cleanup{true};
    bool dry_run{false};
};

struct Stage {
    char label;
    ar::AgentId id;
    std::string name;
    ar::AgentState last_state{ar::AgentState::created};
    bool observed{false};
    std::optional<ar::ControlAgentInfo> info;
};

void usage() {
    std::cout
        << "usage: agent_mql_sql_demo [OPTIONS]\n"
        << "  --schema PATH       PMS schema file (default: ../pms_schema.sql)\n"
        << "  --question TEXT     natural-language database question\n"
        << "  --alias NAME        agent_topd model alias (default: pms)\n"
        << "  --socket PATH       agentd control socket\n"
        << "  --timeout-ms N      whole-DAG timeout (default: 180000)\n"
        << "  --cpu N             requested CPU threads per Agent (default: 4)\n"
        << "  --memory-mib N      admission reservation per Agent (default: 256)\n"
        << "  --id-base N         Agent ID base (default: time-derived)\n"
        << "  --no-cleanup        retain model contexts after the demo\n"
        << "  --dry-run           validate input and print the DAG only\n";
}

template <typename T>
bool parse_unsigned(std::string_view text, T *value) {
    static_assert(std::is_unsigned_v<T>);
    if (text.empty()) return false;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), *value);
    return parsed.ec == std::errc{} &&
           parsed.ptr == text.data() + text.size();
}

bool parse_options(int argc, char **argv, Options *options) {
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help" || option == "-h") {
            usage();
            return false;
        }
        if (option == "--no-cleanup") {
            options->cleanup = false;
            continue;
        }
        if (option == "--dry-run") {
            options->dry_run = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << option << '\n';
            return false;
        }
        const std::string value = argv[++index];
        if (option == "--schema") {
            options->schema_path = value;
        } else if (option == "--question") {
            options->question = value;
        } else if (option == "--alias") {
            options->model_alias = value;
        } else if (option == "--socket") {
            options->socket_path = value;
        } else if (option == "--timeout-ms") {
            if (!parse_unsigned(value, &options->timeout_ms) ||
                options->timeout_ms == 0) {
                std::cerr << "invalid timeout\n";
                return false;
            }
        } else if (option == "--cpu") {
            if (!parse_unsigned(value, &options->cpu_threads) ||
                options->cpu_threads == 0) {
                std::cerr << "invalid CPU thread count\n";
                return false;
            }
        } else if (option == "--memory-mib") {
            if (!parse_unsigned(value, &options->memory_mib) ||
                options->memory_mib == 0 ||
                options->memory_mib >
                    std::numeric_limits<std::uint64_t>::max() /
                        (1024ULL * 1024ULL)) {
                std::cerr << "invalid memory reservation\n";
                return false;
            }
        } else if (option == "--id-base") {
            if (!parse_unsigned(value, &options->id_base) ||
                options->id_base == 0 ||
                options->id_base >
                    std::numeric_limits<ar::AgentId>::max() - 100) {
                std::cerr << "invalid Agent ID base\n";
                return false;
            }
        } else {
            std::cerr << "unknown option: " << option << '\n';
            return false;
        }
    }
    if (options->question.empty() || options->model_alias.empty()) {
        std::cerr << "question and model alias must not be empty\n";
        return false;
    }
    return true;
}

std::optional<std::string> read_schema(const std::filesystem::path &path,
                                       std::string *error) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec) {
        *error = "cannot inspect schema file " + path.string() + ": " +
                 ec.message();
        return std::nullopt;
    }
    if (bytes == 0 || bytes > kMaxSchemaBytes) {
        *error = "schema must contain 1.." +
                 std::to_string(kMaxSchemaBytes) + " bytes";
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "cannot open schema file " + path.string();
        return std::nullopt;
    }
    std::string schema(static_cast<std::size_t>(bytes), '\0');
    input.read(schema.data(), static_cast<std::streamsize>(schema.size()));
    if (!input) {
        *error = "cannot read complete schema file";
        return std::nullopt;
    }
    return schema;
}

std::string json_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + text.size() / 16);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char value : text) {
        switch (value) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (value < 0x20U) {
                    escaped += "\\u00";
                    escaped.push_back(hex[value >> 4U]);
                    escaped.push_back(hex[value & 0x0fU]);
                } else {
                    escaped.push_back(static_cast<char>(value));
                }
        }
    }
    return escaped;
}

std::string model_payload(std::string_view system_prompt,
                          std::string_view prompt,
                          std::uint32_t max_tokens) {
    return "{\"system_prompt\":\"" + json_escape(system_prompt) +
           "\",\"prompt\":\"" + json_escape(prompt) +
           "\",\"max_tokens\":" + std::to_string(max_tokens) +
           ",\"temperature\":0.1,\"top_p\":0.9}";
}

ar::AgentTaskSpec make_model_task(ar::AgentId id,
                                  std::string name,
                                  std::string alias,
                                  std::string payload,
                                  const Options &options,
                                  std::vector<ar::AgentId> dependencies = {}) {
    ar::AgentTaskSpec task;
    task.id = id;
    task.name = std::move(name);
    task.kind = "invocation";
    task.dependencies = std::move(dependencies);
    task.resources.cpu_threads = options.cpu_threads;
    task.resources.memory_bytes =
        options.memory_mib * 1024ULL * 1024ULL;
    task.resources.timeout = std::chrono::milliseconds(options.timeout_ms);
    ar::InvocationSpec invocation;
    invocation.kind = ar::InvocationKind::model;
    invocation.target = std::move(alias);
    invocation.operation = "generate";
    invocation.payload = std::move(payload);
    task.invocation = std::move(invocation);
    return task;
}

bool request(const ar::ControlClient &client,
             const ar::ControlRequest &request_value,
             ar::ControlResponse *response,
             std::string *error) {
    if (!client.request(request_value, response, error)) return false;
    if (!response->success) {
        *error = response->message;
        return false;
    }
    return true;
}

bool submit(const ar::ControlClient &client,
            ar::AgentTaskSpec task,
            std::string *error) {
    ar::ControlRequest request_value;
    request_value.operation = ar::ControlOperation::submit;
    request_value.task = std::move(task);
    ar::ControlResponse response;
    return request(client, request_value, &response, error);
}

std::optional<ar::ControlAgentInfo> status(const ar::ControlClient &client,
                                           ar::AgentId id,
                                           std::string *error) {
    ar::ControlRequest request_value;
    request_value.operation = ar::ControlOperation::status;
    request_value.target_id = id;
    ar::ControlResponse response;
    if (!request(client, request_value, &response, error)) {
        return std::nullopt;
    }
    if (response.agents.size() != 1) {
        *error = "agentd returned an invalid status response";
        return std::nullopt;
    }
    return std::move(response.agents.front());
}

void print_transition(const Stage &stage,
                      const ar::ControlAgentInfo &info,
                      std::chrono::steady_clock::time_point started) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    std::cout << '[' << std::setw(6) << elapsed.count() << " ms] Agent "
              << stage.label << " (" << info.id << ") -> "
              << ar::to_string(info.state);
    if (!info.error.empty()) std::cout << "  " << info.error;
    std::cout << '\n';
}

bool wait_for_stages(const ar::ControlClient &client,
                     std::vector<Stage> *stages,
                     std::uint64_t timeout_ms,
                     bool require_success) {
    const auto started = std::chrono::steady_clock::now();
    const auto deadline =
        started + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        bool all_terminal = true;
        for (Stage &stage : *stages) {
            std::string error;
            auto info = status(client, stage.id, &error);
            if (!info.has_value()) {
                std::cerr << "status " << stage.id << " failed: " << error
                          << '\n';
                return false;
            }
            if (!stage.observed || info->state != stage.last_state) {
                print_transition(stage, *info, started);
                stage.last_state = info->state;
                stage.observed = true;
            }
            stage.info = std::move(info);
            all_terminal = all_terminal &&
                           ar::is_terminal(stage.info->state);
        }
        if (all_terminal) {
            if (!require_success) return true;
            return std::all_of(
                stages->begin(), stages->end(), [](const Stage &stage) {
                    return stage.info.has_value() &&
                           stage.info->state == ar::AgentState::completed;
                });
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "DAG wait timed out after " << timeout_ms << " ms\n";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::optional<std::string> fetch_result(const ar::ControlClient &client,
                                        ar::AgentId id,
                                        bool *truncated,
                                        std::string *error) {
    ar::ControlResult result;
    if (!client.fetch_result(id, &result, error)) return std::nullopt;
    const std::uint64_t end =
        result.reference.offset + result.reference.length;
    if (end < result.reference.offset || end > result.region.size()) {
        *error = "invalid shared-memory result bounds";
        return std::nullopt;
    }
    const auto *begin =
        static_cast<const char *>(result.region.data()) +
        static_cast<std::size_t>(result.reference.offset);
    *truncated = result.truncated();
    return std::string(begin,
                       static_cast<std::size_t>(result.reference.length));
}

void release_result(const ar::ControlClient &client, ar::AgentId id) {
    ar::ControlRequest request_value;
    request_value.operation = ar::ControlOperation::release_result;
    request_value.target_id = id;
    ar::ControlResponse response;
    std::string ignored;
    (void)request(client, request_value, &response, &ignored);
}

bool release_context(const ar::ControlClient &client,
                     const Options &options,
                     ar::AgentId owner_id,
                     ar::ContextId context_id,
                     ar::AgentId cleanup_id,
                     std::string *error) {
    ar::ControlRequest request_value;
    request_value.operation = ar::ControlOperation::spawn;
    request_value.target_id = owner_id;
    request_value.task.id = cleanup_id;
    request_value.task.name = "release-context-" + std::to_string(owner_id);
    request_value.task.kind = "invocation";
    request_value.task.resources.cpu_threads = 1;
    request_value.task.resources.timeout = std::chrono::seconds(10);
    ar::InvocationSpec invocation;
    invocation.kind = ar::InvocationKind::model;
    invocation.target = options.model_alias;
    invocation.operation = "release_context";
    invocation.payload = "{}";
    invocation.context_id = context_id;
    request_value.task.invocation = std::move(invocation);
    ar::ControlResponse response;
    return request(client, request_value, &response, error);
}

void print_summary(const std::vector<Stage> &stages) {
    std::cout << "\nExecution metrics\n"
              << std::left << std::setw(7) << "Agent"
              << std::setw(14) << "State"
              << std::setw(12) << "Context"
              << std::setw(13) << "Exec(us)"
              << std::setw(10) << "Input"
              << std::setw(10) << "Output"
              << std::setw(10) << "Reused"
              << "Prefix\n";
    for (const Stage &stage : stages) {
        if (!stage.info.has_value()) continue;
        const auto &info = *stage.info;
        std::cout << std::left << std::setw(7) << stage.label
                  << std::setw(14) << ar::to_string(info.state)
                  << std::setw(12) << info.context_id
                  << std::setw(13) << info.metrics.execution_time_us
                  << std::setw(10) << info.metrics.input_tokens
                  << std::setw(10) << info.metrics.output_tokens
                  << std::setw(10) << info.metrics.reused_tokens
                  << (info.metrics.cache_hit ? "hit" : "miss") << '\n';
    }
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        return argc > 1 &&
                       (std::string(argv[1]) == "--help" ||
                        std::string(argv[1]) == "-h")
                   ? 0
                   : 2;
    }
    if (!std::filesystem::exists(options.schema_path) &&
        options.schema_path == std::filesystem::path("../pms_schema.sql") &&
        std::filesystem::exists("pms_schema.sql")) {
        options.schema_path = "pms_schema.sql";
    }

    std::string error;
    auto schema = read_schema(options.schema_path, &error);
    if (!schema.has_value()) {
        std::cerr << error << '\n';
        return 1;
    }
    if (options.id_base == 0) {
        const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        options.id_base =
            100000U + static_cast<ar::AgentId>(ticks.count() % 1000000000U);
    }

    const ar::AgentId agent_a = options.id_base + 1;
    const ar::AgentId agent_b = options.id_base + 2;
    const ar::AgentId agent_c = options.id_base + 3;
    const std::string system_prompt =
        "You are one isolated stage in a PMS database query compiler. "
        "Return strict JSON only, without Markdown fences. Preserve exact "
        "database, table, and column identifiers. Never invent an identifier "
        "that is absent from the provided schema or dependency results.";

    const std::string prompt_a =
        "ROLE=A_MQL_PLANNER\n"
        "Convert the user question into database-independent MQL. MQL is a "
        "JSON object with fields intent, metrics, dimensions, filters, "
        "group_by, order_by, and limit. Do not select SQL identifiers.\n"
        "USER_QUESTION:\n" +
        options.question;
    const std::string prompt_b =
        "ROLE=B_SCHEMA_SELECTOR\n"
        "Given the user question and the complete MySQL schema, return the "
        "smallest sufficient sub-schema as JSON. Include databases, tables, "
        "columns with types, primary keys, and inferred join edges. Do not "
        "write SQL.\nUSER_QUESTION:\n" +
        options.question + "\nFULL_SCHEMA_BEGIN\n" + *schema +
        "\nFULL_SCHEMA_END";
    const std::string prompt_c =
        "ROLE=C_SQL_COMPILER\n"
        "Compile the two dependency results into one read-only MySQL SELECT. "
        "Dependency producer " +
        std::to_string(agent_a) +
        " is A's MQL JSON. Dependency producer " +
        std::to_string(agent_b) +
        " is B's sub-schema JSON. Return JSON with fields sql, parameters, "
        "and explanation. Use only identifiers present in B's result and "
        "satisfy A's semantics.\nUSER_QUESTION:\n" +
        options.question;

    const auto task_a = make_model_task(
        agent_a,
        "question-to-mql",
        options.model_alias,
        model_payload(system_prompt, prompt_a, 512),
        options);
    const auto task_b = make_model_task(
        agent_b,
        "schema-to-sub-schema",
        options.model_alias,
        model_payload(system_prompt, prompt_b, 1024),
        options);
    const auto task_c = make_model_task(
        agent_c,
        "mql-sub-schema-to-sql",
        options.model_alias,
        model_payload(system_prompt, prompt_c, 768),
        options,
        {agent_a, agent_b});

    std::cout << "PMS MQL-to-SQL DAG\n"
              << "schema=" << options.schema_path
              << " (" << schema->size() << " bytes)\n"
              << "question=" << options.question << "\n"
              << "A[" << agent_a << "] question -> MQL       \\\n"
              << "                                         -> C[" << agent_c
              << "] MQL + sub-schema -> SQL\n"
              << "B[" << agent_b
              << "] question + schema -> sub-schema /\n";
    if (options.dry_run) {
        std::cout << "dry-run: inputs and DAG are valid\n";
        return 0;
    }

    ar::ControlClient client(options.socket_path);
    ar::ControlRequest ping;
    ping.operation = ar::ControlOperation::ping;
    ar::ControlResponse ping_response;
    if (!request(client, ping, &ping_response, &error)) {
        std::cerr << "agentd is unavailable: " << error << '\n';
        return 1;
    }

    // Submit A and B without dependencies before C. With at least two agentd
    // workers they are independently dispatchable; C remains blocked on both.
    if (!submit(client, task_a, &error) ||
        !submit(client, task_b, &error) ||
        !submit(client, task_c, &error)) {
        std::cerr << "cannot submit DAG: " << error << '\n';
        return 1;
    }

    std::vector<Stage> stages{
        {'A', agent_a, "question-to-mql"},
        {'B', agent_b, "schema-to-sub-schema"},
        {'C', agent_c, "mql-sub-schema-to-sql"}};
    const bool completed =
        wait_for_stages(client, &stages, options.timeout_ms, true);
    print_summary(stages);

    if (completed) {
        for (const Stage &stage : stages) {
            bool truncated = false;
            auto output =
                fetch_result(client, stage.id, &truncated, &error);
            std::cout << "\nAgent " << stage.label << " output\n";
            if (!output.has_value()) {
                std::cout << "<unavailable: " << error << ">\n";
                continue;
            }
            std::cout << *output << '\n';
            if (truncated) std::cout << "[output truncated]\n";
        }
    }

    if (options.cleanup) {
        std::vector<Stage> cleanup_stages;
        ar::AgentId cleanup_id = options.id_base + 11;
        for (const Stage &stage : stages) {
            if (!stage.info.has_value() ||
                stage.info->context_id == ar::kInvalidContextId) {
                continue;
            }
            if (!release_context(client,
                                 options,
                                 stage.id,
                                 stage.info->context_id,
                                 cleanup_id,
                                 &error)) {
                std::cerr << "cannot schedule context cleanup for Agent "
                          << stage.label << ": " << error << '\n';
                ++cleanup_id;
                continue;
            }
            cleanup_stages.push_back(
                {'R', cleanup_id, "release-context"});
            ++cleanup_id;
        }
        if (!cleanup_stages.empty()) {
            (void)wait_for_stages(
                client, &cleanup_stages, 15000, false);
            for (const Stage &stage : cleanup_stages) {
                release_result(client, stage.id);
            }
        }
    }
    for (const Stage &stage : stages) release_result(client, stage.id);
    return completed ? 0 : 1;
}
