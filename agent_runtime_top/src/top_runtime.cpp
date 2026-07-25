#include "top_runtime.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <type_traits>
#include <utility>

namespace agent_runtime_top {
namespace {

constexpr std::size_t kSharedPrefixSlots = 8;

std::size_t logical_context_limit(const TopRuntimeConfig &config) {
    if (!config.enable_context_swap) return config.max_contexts;
    if (config.max_swapped_contexts >
        std::numeric_limits<std::size_t>::max() - config.max_contexts) {
        return std::numeric_limits<std::size_t>::max();
    }
    return config.max_contexts + config.max_swapped_contexts;
}

void set_error(std::string *error, const std::string &message) {
    if (error != nullptr) *error = message;
}

std::size_t skip_space(const std::string &text, std::size_t position) {
    while (position < text.size() &&
           std::isspace(static_cast<unsigned char>(text[position])) != 0) {
        ++position;
    }
    return position;
}

std::optional<std::size_t> json_value_position(
    const std::string &json,
    const std::string &key) {
    const std::string marker = "\"" + key + "\"";
    std::size_t position = json.find(marker);
    if (position == std::string::npos) return std::nullopt;
    position = skip_space(json, position + marker.size());
    if (position >= json.size() || json[position] != ':') {
        return std::nullopt;
    }
    return skip_space(json, position + 1);
}

std::optional<std::string> json_string(const std::string &json,
                                       const std::string &key) {
    const auto value_position = json_value_position(json, key);
    if (!value_position.has_value() || *value_position >= json.size() ||
        json[*value_position] != '"') {
        return std::nullopt;
    }
    std::string value;
    for (std::size_t position = *value_position + 1;
         position < json.size();
         ++position) {
        const char current = json[position];
        if (current == '"') return value;
        if (current != '\\') {
            value.push_back(current);
            continue;
        }
        if (++position >= json.size()) return std::nullopt;
        switch (json[position]) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> json_number(const std::string &json,
                             const std::string &key) {
    const auto value_position = json_value_position(json, key);
    if (!value_position.has_value()) return std::nullopt;
    const char *begin = json.c_str() + *value_position;
    char *end = nullptr;
    if constexpr (std::is_integral_v<T>) {
        const long long value = std::strtoll(begin, &end, 10);
        if (end == begin ||
            value < static_cast<long long>(std::numeric_limits<T>::min()) ||
            value > static_cast<long long>(std::numeric_limits<T>::max())) {
            return std::nullopt;
        }
        return static_cast<T>(value);
    } else {
        const float value = std::strtof(begin, &end);
        if (end == begin) return std::nullopt;
        return static_cast<T>(value);
    }
}

std::string mapped_text(
    const agent_runtime::InvocationMappedInput &input) {
    const auto type =
        static_cast<agent_runtime::SharedDataType>(
            input.reference.data_type);
    if (type != agent_runtime::SharedDataType::text_utf8 &&
        type != agent_runtime::SharedDataType::json_utf8) {
        return {};
    }
    const auto *begin =
        static_cast<const char *>(input.region.data()) +
        static_cast<std::size_t>(input.reference.offset);
    return std::string(
        begin, static_cast<std::size_t>(input.reference.length));
}

bool cancel_adapter(void *user_data) {
    const auto *callback =
        static_cast<const std::function<bool()> *>(user_data);
    return callback != nullptr && (*callback)();
}

class ContextPinGuard {
public:
    ContextPinGuard(ContextManager *contexts, ContextId context_id)
        : contexts_(contexts), context_id_(context_id) {}
    ~ContextPinGuard() { finish(); }

    ContextPinGuard(const ContextPinGuard &) = delete;
    ContextPinGuard &operator=(const ContextPinGuard &) = delete;

    void finish() {
        if (contexts_ == nullptr) return;
        (void) contexts_->finish(context_id_);
        contexts_ = nullptr;
    }

private:
    ContextManager *contexts_;
    ContextId context_id_;
};

}  // namespace

TopRuntime::TopRuntime(TopRuntimeConfig config)
    : config_(std::move(config)),
      contexts_(
          config_.max_contexts,
          logical_context_limit(config_)),
      tools_(config_.enable_shell_tool) {}

TopRuntime::~TopRuntime() {
    shutdown();
}

bool TopRuntime::initialize_snapshot_store(std::string *error) {
    if (!config_.enable_context_swap) return true;

    namespace fs = std::filesystem;
    fs::path base;
    if (!config_.swap_directory.empty()) {
        base = config_.swap_directory;
    } else {
        if (const char *runtime_directory = std::getenv("XDG_RUNTIME_DIR");
            runtime_directory != nullptr && *runtime_directory != '\0') {
            base = runtime_directory;
        } else {
            std::error_code temporary_error;
            base = fs::temp_directory_path(temporary_error);
            if (temporary_error) {
                set_error(error, "cannot locate a temporary directory");
                return false;
            }
        }
    }
    const auto nonce =
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count()) ^
        static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(this));
    const fs::path directory =
        base / ("agentnucleus-contexts-" + std::to_string(nonce));
    owns_swap_directory_ = true;

    std::error_code filesystem_error;
    fs::create_directories(directory, filesystem_error);
    const bool is_directory =
        !filesystem_error &&
        fs::is_directory(directory, filesystem_error);
    if (filesystem_error || !is_directory) {
        std::error_code ignored;
        fs::remove(directory, ignored);
        owns_swap_directory_ = false;
        set_error(error,
                  "cannot create context swap directory: " +
                      directory.string());
        return false;
    }
    fs::permissions(
        directory,
        fs::perms::owner_all,
        fs::perm_options::replace,
        filesystem_error);
    if (filesystem_error) {
        std::error_code ignored;
        fs::remove(directory, ignored);
        owns_swap_directory_ = false;
        set_error(error,
                  "cannot secure context swap directory: " +
                      directory.string());
        return false;
    }
    active_swap_directory_ = directory.string();
    return true;
}

bool TopRuntime::swap_out_context(ContextId context_id,
                                  int sequence_id,
                                  std::string *error) {
    namespace fs = std::filesystem;
    std::lock_guard lock(snapshot_mutex_);
    if (active_swap_directory_.empty()) {
        set_error(error, "context swap is not initialized");
        return false;
    }

    const fs::path final_path =
        fs::path(active_swap_directory_) /
        ("context-" + std::to_string(context_id) + ".bin");
    const fs::path temporary_path =
        final_path.string() + ".tmp";
    std::error_code filesystem_error;
    fs::remove(temporary_path, filesystem_error);
    filesystem_error.clear();

    int position = 0;
    std::uint64_t bytes = 0;
    if (llm_checkpoint_seq(model_id_,
                           sequence_id,
                           temporary_path.string().c_str(),
                           &position,
                           &bytes) != 0) {
        set_error(error, "cannot checkpoint llama sequence");
        return false;
    }
    fs::permissions(
        temporary_path,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace,
        filesystem_error);
    if (filesystem_error) {
        fs::remove(temporary_path, filesystem_error);
        set_error(error, "cannot secure context checkpoint");
        return false;
    }
    fs::remove(final_path, filesystem_error);
    filesystem_error.clear();
    fs::rename(temporary_path, final_path, filesystem_error);
    if (filesystem_error) {
        fs::remove(temporary_path, filesystem_error);
        set_error(error, "cannot publish context checkpoint atomically");
        return false;
    }

    if (const auto old = snapshots_.find(context_id);
        old != snapshots_.end()) {
        snapshot_bytes_ -= old->second.bytes;
    }
    snapshots_[context_id] = Snapshot{
        .path = final_path.string(),
        .position = position,
        .bytes = bytes};
    snapshot_bytes_ += bytes;
    llm_clear_seq(model_id_, sequence_id);
    return true;
}

bool TopRuntime::swap_in_context(ContextId context_id,
                                 int sequence_id,
                                 std::string *error) {
    namespace fs = std::filesystem;
    std::lock_guard lock(snapshot_mutex_);
    const auto snapshot = snapshots_.find(context_id);
    if (snapshot == snapshots_.end()) {
        set_error(error, "context checkpoint does not exist");
        return false;
    }

    std::uint64_t bytes = 0;
    if (llm_restore_seq(model_id_,
                        sequence_id,
                        snapshot->second.path.c_str(),
                        snapshot->second.position,
                        &bytes) != 0) {
        set_error(error, "cannot restore llama sequence checkpoint");
        return false;
    }
    std::error_code filesystem_error;
    fs::remove(snapshot->second.path, filesystem_error);
    if (!filesystem_error) {
        snapshot_bytes_ -= snapshot->second.bytes;
        snapshots_.erase(snapshot);
    }
    return true;
}

void TopRuntime::remove_snapshot(ContextId context_id) {
    namespace fs = std::filesystem;
    std::lock_guard lock(snapshot_mutex_);
    const auto snapshot = snapshots_.find(context_id);
    if (snapshot == snapshots_.end()) return;
    std::error_code ignored;
    fs::remove(snapshot->second.path, ignored);
    snapshot_bytes_ -= snapshot->second.bytes;
    snapshots_.erase(snapshot);
}

void TopRuntime::cleanup_snapshots() {
    namespace fs = std::filesystem;
    std::lock_guard lock(snapshot_mutex_);
    std::error_code ignored;
    for (const auto &[context_id, snapshot] : snapshots_) {
        (void) context_id;
        fs::remove(snapshot.path, ignored);
        ignored.clear();
    }
    snapshots_.clear();
    snapshot_bytes_ = 0;
    if (owns_swap_directory_ && !active_swap_directory_.empty()) {
        fs::remove(active_swap_directory_, ignored);
    }
    active_swap_directory_.clear();
    owns_swap_directory_ = false;
}

bool TopRuntime::format_chat(
    const std::vector<std::pair<std::string, std::string>> &messages,
    bool add_assistant_prompt,
    std::string *formatted) const {
    if (formatted == nullptr || messages.empty()) return false;
    std::vector<llm_chat_message_value> native_messages;
    native_messages.reserve(messages.size());
    for (const auto &[role, content] : messages) {
        native_messages.push_back(
            {.role = role.c_str(), .content = content.c_str()});
    }

    char *output = nullptr;
    std::uint32_t length = 0;
    const int result = llm_apply_chat_template(
        model_id_,
        native_messages.data(),
        static_cast<std::uint32_t>(native_messages.size()),
        add_assistant_prompt,
        &output,
        &length);
    if (result != 0 || output == nullptr) {
        llm_free_text(output);
        return false;
    }
    formatted->assign(output, length);
    llm_free_text(output);
    return true;
}

bool TopRuntime::initialize(std::string *error) {
    if (error != nullptr) error->clear();
    std::lock_guard lock(lifecycle_mutex_);
    if (initialized_) {
        set_error(error, "top runtime is already initialized");
        return false;
    }
    if (config_.model_alias.empty() ||
        config_.model_path.empty() ||
        config_.model_path.size() >= LLM_MAX_MODEL_PATH ||
        config_.context_tokens == 0 ||
        config_.context_tokens >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        config_.batch_tokens == 0 ||
        config_.batch_tokens >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        config_.cpu_threads == 0 ||
        config_.cpu_threads >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        config_.max_tokens == 0 ||
        config_.max_tokens > config_.context_tokens ||
        config_.temperature <= 0.0F || config_.temperature > 2.0F ||
        config_.top_p <= 0.0F || config_.top_p > 1.0F ||
        config_.max_contexts == 0 ||
        (config_.enable_context_swap &&
         config_.max_swapped_contexts == 0) ||
        logical_context_limit(config_) ==
            std::numeric_limits<std::size_t>::max() ||
        config_.max_contexts >
            64 - kSharedPrefixSlots - 1) {
        set_error(error, "invalid top runtime model/context configuration");
        return false;
    }
    if (llm_manager_init() != 0) {
        set_error(error, "cannot initialize the LLM manager");
        return false;
    }

    llm_model_config model_config{};
    std::snprintf(model_config.model_path,
                  sizeof(model_config.model_path),
                  "%s",
                  config_.model_path.c_str());
    model_config.n_ctx = static_cast<int>(config_.context_tokens);
    model_config.n_batch = static_cast<int>(config_.batch_tokens);
    model_config.n_seq_max = static_cast<int>(
        config_.max_contexts + kSharedPrefixSlots + 1);
    model_config.n_threads = static_cast<int>(config_.cpu_threads);
    model_config.n_gpu_layers = config_.gpu_layers;
    model_config.temperature = config_.temperature;
    model_config.top_p = config_.top_p;
    model_config.max_tokens = static_cast<int>(config_.max_tokens);
    model_config.use_mmap = config_.use_mmap;
    model_config.use_mlock = config_.use_mlock;
    model_config.kv_type_k = config_.kv_type_k;
    model_config.kv_type_v = config_.kv_type_v;

    if (llm_load_model(
            LLM_BACKEND_LLAMA_CPP, &model_config, &model_id_) != 0) {
        llm_manager_destroy();
        set_error(error, "cannot load model: " + config_.model_path);
        return false;
    }
    if (!initialize_snapshot_store(error)) {
        llm_unload_model(model_id_);
        model_id_ = 0;
        llm_manager_destroy();
        return false;
    }
    initialized_ = true;
    return true;
}

void TopRuntime::shutdown() {
    std::lock_guard lock(lifecycle_mutex_);
    if (!initialized_) return;
    cleanup_snapshots();
    {
        std::lock_guard conversation_lock(conversation_mutex_);
        conversations_.clear();
    }
    llm_unload_model(model_id_);
    model_id_ = 0;
    llm_manager_destroy();
    initialized_ = false;
}

agent_runtime::InvocationHandlerResult TopRuntime::make_text_result(
    std::string text,
    agent_runtime::SharedDataType type,
    std::string *error) const {
    agent_runtime::InvocationHandlerResult result;
    const std::size_t region_size = std::max<std::size_t>(1, text.size());
    result.output =
        agent_runtime::SharedMemoryRegion::create(region_size, error);
    if (!result.output.valid()) {
        result.status = agent_runtime::InvocationStatus::failed;
        result.message =
            error != nullptr ? *error : "cannot allocate invocation output";
        return result;
    }
    if (!text.empty()) {
        std::memcpy(result.output.data(), text.data(), text.size());
    }
    result.output_length = text.size();
    result.output_type = type;
    return result;
}

agent_runtime::InvocationHandlerResult TopRuntime::handle(
    const agent_runtime::InvocationRequest &request,
    const std::vector<agent_runtime::InvocationMappedInput> &inputs,
    const std::function<bool()> &cancel_requested) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (!initialized_) {
        agent_runtime::InvocationHandlerResult result;
        result.status = agent_runtime::InvocationStatus::failed;
        result.message = "top runtime is not initialized";
        return result;
    }
    if (cancel_requested()) {
        agent_runtime::InvocationHandlerResult result;
        result.status = agent_runtime::InvocationStatus::cancelled;
        result.message = "invocation cancelled before execution";
        return result;
    }
    if (request.kind == agent_runtime::InvocationKind::model) {
        return handle_model(request, inputs, cancel_requested);
    }
    if (request.kind == agent_runtime::InvocationKind::tool) {
        return handle_tool(request, inputs, cancel_requested);
    }
    agent_runtime::InvocationHandlerResult result;
    result.status = agent_runtime::InvocationStatus::rejected;
    result.message = "unsupported invocation kind";
    return result;
}

agent_runtime::InvocationHandlerResult TopRuntime::handle_model(
    const agent_runtime::InvocationRequest &request,
    const std::vector<agent_runtime::InvocationMappedInput> &inputs,
    const std::function<bool()> &cancel_requested) {
    if (request.target != config_.model_alias) {
        agent_runtime::InvocationHandlerResult result;
        result.status = agent_runtime::InvocationStatus::rejected;
        result.message = "unknown model alias: " + request.target;
        return result;
    }

    if (request.operation == "release_context") {
        int released_sequence = -1;
        bool context_removed = false;
        std::string error;
        if (request.context_id == 0 ||
            !contexts_.release(request.agent_id,
                               request.parent_id,
                               request.context_id,
                               &released_sequence,
                               &error,
                               &context_removed)) {
            agent_runtime::InvocationHandlerResult result;
            result.status = agent_runtime::InvocationStatus::rejected;
            result.message = error.empty() ? "context id is required" : error;
            return result;
        }
        if (released_sequence >= 0) {
            llm_clear_seq(model_id_, released_sequence);
        }
        if (context_removed) {
            remove_snapshot(request.context_id);
            std::lock_guard conversation_lock(conversation_mutex_);
            conversations_.erase(request.context_id);
        }
        return make_text_result(
            R"({"released":true})",
            agent_runtime::SharedDataType::json_utf8);
    }

    if (request.operation == "stats") {
        llm_stats llm_statistics{};
        int shares = 0;
        int saved_tokens = 0;
        (void) llm_get_stats(model_id_, &llm_statistics);
        llm_get_shared_kv_stats(&shares, &saved_tokens);
        const ContextManagerStats context_statistics = contexts_.stats();
        std::size_t snapshot_count = 0;
        std::uint64_t snapshot_bytes = 0;
        {
            std::lock_guard lock(snapshot_mutex_);
            snapshot_count = snapshots_.size();
            snapshot_bytes = snapshot_bytes_;
        }
        std::ostringstream json;
        json << "{\"requests\":" << llm_statistics.total_requests
             << ",\"tokens\":" << llm_statistics.total_tokens
             << ",\"prefix_evictions\":"
             << llm_statistics.prefix_evictions
             << ",\"avg_latency_ms\":" << llm_statistics.avg_latency_ms
             << ",\"tokens_per_second\":"
             << llm_statistics.tokens_per_second
             << ",\"shared_hits\":" << shares
             << ",\"reused_tokens\":" << saved_tokens
             << ",\"active_contexts\":"
             << context_statistics.active_contexts
             << ",\"active_owners\":" << context_statistics.active_owners
             << ",\"resident_contexts\":"
             << context_statistics.resident_contexts
             << ",\"swapped_contexts\":"
             << context_statistics.swapped_contexts
             << ",\"swap_outs\":" << context_statistics.swap_outs
             << ",\"swap_ins\":" << context_statistics.swap_ins
             << ",\"snapshot_count\":" << snapshot_count
             << ",\"snapshot_bytes\":" << snapshot_bytes
             << "}";
        return make_text_result(
            json.str(), agent_runtime::SharedDataType::json_utf8);
    }

    if (request.operation == "warmup_prefix") {
        std::string system_prompt = request.payload;
        const std::size_t first = skip_space(request.payload, 0);
        if (first < request.payload.size() &&
            request.payload[first] == '{') {
            system_prompt =
                json_string(request.payload, "system_prompt").value_or("");
        }
        if (system_prompt.empty() ||
            system_prompt.size() > LLM_MAX_PROMPT) {
            agent_runtime::InvocationHandlerResult result;
            result.status = agent_runtime::InvocationStatus::rejected;
            result.message = "warmup_prefix requires a valid system prompt";
            return result;
        }
        if (config_.use_chat_template) {
            std::string formatted_system;
            if (format_chat(
                    {{"system", system_prompt}},
                    false,
                    &formatted_system)) {
                system_prompt = std::move(formatted_system);
            }
        }
        const int sequence = llm_warmup_shared_kv_with_cancel(
            model_id_,
            system_prompt.c_str(),
            cancel_adapter,
            const_cast<std::function<bool()> *>(&cancel_requested));
        if (sequence < 0) {
            agent_runtime::InvocationHandlerResult result;
            result.status =
                sequence == -2 || cancel_requested()
                    ? agent_runtime::InvocationStatus::cancelled
                    : agent_runtime::InvocationStatus::failed;
            result.message =
                result.status == agent_runtime::InvocationStatus::cancelled
                    ? "shared prefix warmup cancelled"
                    : "shared prefix encoding failed";
            return result;
        }
        return make_text_result(
            "{\"warmed\":true,\"reserved_sequence\":" +
                std::to_string(sequence) + "}",
            agent_runtime::SharedDataType::json_utf8);
    }

    if (request.operation != "generate") {
        agent_runtime::InvocationHandlerResult result;
        result.status = agent_runtime::InvocationStatus::rejected;
        result.message =
            "model operation must be generate, warmup_prefix, stats, "
            "or release_context";
        return result;
    }

    std::string prompt;
    std::string system_prompt;
    int max_tokens = static_cast<int>(config_.max_tokens);
    float temperature = config_.temperature;
    float top_p = config_.top_p;
    const std::size_t first =
        skip_space(request.payload, 0);
    if (first < request.payload.size() && request.payload[first] == '{') {
        prompt = json_string(request.payload, "prompt").value_or("");
        system_prompt =
            json_string(request.payload, "system_prompt").value_or("");
        max_tokens =
            json_number<int>(request.payload, "max_tokens")
                .value_or(max_tokens);
        temperature =
            json_number<float>(request.payload, "temperature")
                .value_or(temperature);
        top_p =
            json_number<float>(request.payload, "top_p").value_or(top_p);
    } else {
        prompt = request.payload;
    }
    for (const auto &input : inputs) {
        if (input.reference.length > LLM_MAX_PROMPT) {
            agent_runtime::InvocationHandlerResult result;
            result.status = agent_runtime::InvocationStatus::rejected;
            result.message = "model dependency input exceeds prompt limit";
            return result;
        }
        const std::string dependency = mapped_text(input);
        if (dependency.empty()) continue;
        const std::string marker =
            "\n[dependency " + std::to_string(input.producer_id) + "]\n";
        if (prompt.size() > LLM_MAX_PROMPT ||
            marker.size() > LLM_MAX_PROMPT - prompt.size() ||
            dependency.size() >
                LLM_MAX_PROMPT - prompt.size() - marker.size()) {
            agent_runtime::InvocationHandlerResult result;
            result.status = agent_runtime::InvocationStatus::rejected;
            result.message = "combined model prompt exceeds size limit";
            return result;
        }
        prompt.append(marker);
        prompt.append(dependency);
    }
    if (prompt.empty() || prompt.size() > LLM_MAX_PROMPT ||
        system_prompt.size() > LLM_MAX_PROMPT ||
        max_tokens <= 0 || max_tokens > 4096 ||
        temperature < 0.0F || temperature > 2.0F ||
        top_p <= 0.0F || top_p > 1.0F) {
        agent_runtime::InvocationHandlerResult result;
        result.status = agent_runtime::InvocationStatus::rejected;
        result.message = "invalid model generation payload";
        return result;
    }

    std::unique_lock conversation_lock(conversation_mutex_);
    std::string context_error;
    ContextSwapCallbacks swap_callbacks;
    const ContextSwapCallbacks *swap = nullptr;
    if (config_.enable_context_swap) {
        swap_callbacks.save =
            [this](ContextId context_id,
                   int sequence_id,
                   std::string *error) {
                return swap_out_context(context_id, sequence_id, error);
            };
        swap_callbacks.load =
            [this](ContextId context_id,
                   int sequence_id,
                   std::string *error) {
                return swap_in_context(context_id, sequence_id, error);
            };
        swap = &swap_callbacks;
    }
    auto lease = contexts_.acquire(request.agent_id,
                                   request.parent_id,
                                   request.context_id,
                                   &context_error,
                                   swap);
    if (!lease.has_value()) {
        agent_runtime::InvocationHandlerResult result;
        result.status = agent_runtime::InvocationStatus::busy;
        result.message = context_error;
        return result;
    }
    ContextPinGuard context_pin(&contexts_, lease->context_id);

    auto [conversation_it, inserted] =
        conversations_.try_emplace(lease->context_id);
    Conversation &conversation = conversation_it->second;
    if (lease->created) {
        conversation = Conversation{};
        conversation.template_active = config_.use_chat_template;
    } else if (inserted) {
        conversation.template_active = false;
    }
    const std::size_t previous_message_count =
        conversation.messages.size();
    std::string inference_prompt = prompt;
    std::string inference_system_prompt = system_prompt;
    std::string fully_formatted_prompt;
    std::string previous_formatted_prompt;
    bool chat_template_applied = false;

    if (conversation.template_active) {
        if (!lease->created &&
            !format_chat(conversation.messages,
                         false,
                         &previous_formatted_prompt)) {
            agent_runtime::InvocationHandlerResult result;
            result.status = agent_runtime::InvocationStatus::failed;
            result.message = "cannot render previous chat history";
            return result;
        }
        if (lease->created && !system_prompt.empty()) {
            conversation.messages.emplace_back(
                "system", system_prompt);
        }
        conversation.messages.emplace_back("user", prompt);
        if (format_chat(
                conversation.messages,
                true,
                &fully_formatted_prompt)) {
            if (lease->created) {
                inference_system_prompt.clear();
                inference_prompt = fully_formatted_prompt;
                if (!system_prompt.empty()) {
                    std::string formatted_system;
                    if (format_chat(
                            {{"system", system_prompt}},
                            false,
                            &formatted_system) &&
                        fully_formatted_prompt.starts_with(
                            formatted_system)) {
                        inference_system_prompt =
                            std::move(formatted_system);
                        inference_prompt =
                            fully_formatted_prompt.substr(
                                inference_system_prompt.size());
                    }
                }
                chat_template_applied = true;
            } else if (fully_formatted_prompt.starts_with(
                           previous_formatted_prompt)) {
                inference_system_prompt.clear();
                inference_prompt = fully_formatted_prompt.substr(
                    previous_formatted_prompt.size());
                // This matches llama.cpp's incremental-chat helper: some
                // templates consume a trailing newline while rendering past
                // messages, so it must be reintroduced before the new turn.
                if (!previous_formatted_prompt.empty() &&
                    previous_formatted_prompt.back() == '\n') {
                    inference_prompt.insert(inference_prompt.begin(), '\n');
                }
                chat_template_applied = true;
            } else {
                conversation.messages.resize(previous_message_count);
                agent_runtime::InvocationHandlerResult result;
                result.status =
                    agent_runtime::InvocationStatus::failed;
                result.message =
                    "chat template history became inconsistent";
                return result;
            }
        } else if (lease->created) {
            conversation.messages.clear();
            conversation.template_active = false;
        } else {
            conversation.messages.resize(previous_message_count);
            agent_runtime::InvocationHandlerResult result;
            result.status = agent_runtime::InvocationStatus::failed;
            result.message = "cannot extend chat template history";
            return result;
        }
    }

    llm_request llm_request_value{};
    llm_request_value.prompt = inference_prompt.c_str();
    llm_request_value.system_prompt =
        inference_system_prompt.empty()
            ? nullptr
            : inference_system_prompt.c_str();
    llm_request_value.seq_id = lease->sequence_id;
    llm_request_value.is_first_turn = lease->created;
    llm_request_value.close_turn = chat_template_applied;
    llm_request_value.cpu_threads = static_cast<int>(
        std::min<std::uint32_t>(
            request.resources.cpu_threads, config_.cpu_threads));
    llm_request_value.temperature = temperature;
    llm_request_value.top_p = top_p;
    llm_request_value.max_tokens = max_tokens;
    llm_request_value.cancel_callback = cancel_adapter;
    llm_request_value.cancel_user_data =
        const_cast<std::function<bool()> *>(&cancel_requested);

    llm_response llm_response_value{};
    const int inference_result =
        llm_inference(model_id_, &llm_request_value, &llm_response_value);
    context_pin.finish();
    if (inference_result != 0) {
        if (chat_template_applied) {
            conversation.messages.resize(previous_message_count);
        }
        if (lease->created) {
            int released_sequence = -1;
            std::string ignored;
            if (contexts_.release(request.agent_id,
                                  request.parent_id,
                                  lease->context_id,
                                  &released_sequence,
                                  &ignored,
                                  nullptr) &&
                released_sequence >= 0) {
                llm_clear_seq(model_id_, released_sequence);
            }
            conversations_.erase(lease->context_id);
        }
        agent_runtime::InvocationHandlerResult result;
        result.status =
            cancel_requested() || llm_response_value.cancelled
                ? agent_runtime::InvocationStatus::cancelled
                : agent_runtime::InvocationStatus::failed;
        result.message = result.status ==
                                 agent_runtime::InvocationStatus::cancelled
                             ? "model generation cancelled"
                             : "model generation failed";
        llm_free_response(&llm_response_value);
        return result;
    }

    std::string output =
        llm_response_value.text != nullptr ? llm_response_value.text : "";
    if (chat_template_applied) {
        conversation.messages.emplace_back("assistant", output);
    }
    std::string output_error;
    auto result = make_text_result(
        std::move(output),
        agent_runtime::SharedDataType::text_utf8,
        &output_error);
    result.context_id = lease->context_id;
    result.metrics.execution_time_us = static_cast<std::uint64_t>(
        std::max(0.0F, llm_response_value.eval_time_ms) * 1000.0F);
    result.metrics.input_tokens =
        static_cast<std::uint64_t>(
            std::max(0, llm_response_value.prompt_tokens));
    result.metrics.output_tokens =
        static_cast<std::uint64_t>(
            std::max(0, llm_response_value.tokens_generated));
    result.metrics.reused_tokens =
        static_cast<std::uint64_t>(
            std::max(0, llm_response_value.reused_tokens));
    result.metrics.cache_bytes = llm_response_value.cache_bytes;
    result.metrics.cache_hit = llm_response_value.cache_hit;
    llm_free_response(&llm_response_value);
    return result;
}

agent_runtime::InvocationHandlerResult TopRuntime::handle_tool(
    const agent_runtime::InvocationRequest &request,
    const std::vector<agent_runtime::InvocationMappedInput> &inputs,
    const std::function<bool()> &cancel_requested) {
    ToolResult tool_result =
        tools_.execute(request, inputs, cancel_requested);
    std::string output_error;
    auto result = make_text_result(
        std::move(tool_result.output),
        tool_result.output_type,
        &output_error);
    if (tool_result.status != agent_runtime::InvocationStatus::ok) {
        result.status = tool_result.status;
        result.message = std::move(tool_result.message);
    }
    result.process_id = tool_result.process_id;
    result.metrics.execution_time_us = tool_result.execution_time_us;
    if (tool_result.output_truncated) {
        result.output_flags |= agent_runtime::shared_buffer_truncated;
    }
    return result;
}

}  // namespace agent_runtime_top
