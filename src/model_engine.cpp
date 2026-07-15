#include "agent_runtime/model_engine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

#ifdef AGENT_RUNTIME_WITH_LLAMA
#include "llama.h"
#endif

namespace agent_runtime {
namespace {

#ifdef AGENT_RUNTIME_WITH_LLAMA
constexpr std::uint64_t kContextSnapshotMagic = 0x4152544354583031ULL;
constexpr std::uint32_t kContextSnapshotVersion = 1;
constexpr AgentId kModelReservationId =
    std::numeric_limits<AgentId>::max();

struct ContextSnapshotHeader {
    std::uint64_t magic{kContextSnapshotMagic};
    std::uint32_t version{kContextSnapshotVersion};
    std::uint32_t reserved{0};
    std::uint64_t state_size{0};
};
#endif

}  // namespace

struct ModelEngine::ContextSlot {
    ContextId id{kInvalidContextId};
    AgentId owner{kInvalidAgentId};
    llama_context *context{nullptr};
    std::atomic_bool cancel_requested{false};
    std::string snapshot_path;
    std::chrono::steady_clock::time_point last_active;
};

ModelEngine::ModelEngine(ModelEngineConfig config)
    : config_(std::move(config)), admission_(config_.safety_margin_bytes) {}

ModelEngine::~ModelEngine() {
    std::lock_guard lock(mutex_);
#ifdef AGENT_RUNTIME_WITH_LLAMA
    for (auto &[id, slot] : contexts_) {
        if (slot->context != nullptr) {
            llama_free(slot->context);
        }
        admission_.release(id);
    }
    contexts_.clear();
    if (model_ != nullptr) {
        llama_model_free(model_);
        model_ = nullptr;
    }
    if (backend_initialized_) {
        llama_backend_free();
        backend_initialized_ = false;
    }
#endif
}

bool ModelEngine::initialize(std::string *error) {
    std::lock_guard lock(mutex_);
#ifndef AGENT_RUNTIME_WITH_LLAMA
    if (error != nullptr) {
        *error = "llama.cpp support is disabled; configure with "
                 "-DAGENT_RUNTIME_WITH_LLAMA=ON";
    }
    return false;
#else
    if (model_ != nullptr) {
        return true;
    }
    if (config_.model_path.empty()) {
        if (error != nullptr) *error = "model path is empty";
        return false;
    }

    std::error_code file_error;
    const std::uintmax_t model_file_size =
        std::filesystem::file_size(config_.model_path, file_error);
    if (file_error || model_file_size > std::numeric_limits<std::uint64_t>::max()) {
        if (error != nullptr) *error = "cannot inspect model file";
        return false;
    }
    const std::uint64_t model_reservation = config_.model_reservation_bytes == 0
        ? static_cast<std::uint64_t>(model_file_size)
        : config_.model_reservation_bytes;
    const auto memory = monitor_.sample(error);
    if (!memory.has_value() ||
        !admission_.try_reserve(
            kModelReservationId, model_reservation, *memory, error)) {
        return false;
    }

    llama_backend_init();
    backend_initialized_ = true;
    llama_model_params parameters = llama_model_default_params();
    parameters.n_gpu_layers = 0;
    parameters.use_mmap = true;
    parameters.use_mlock = false;
    model_ = llama_model_load_from_file(config_.model_path.c_str(), parameters);
    admission_.release(kModelReservationId);
    if (model_ == nullptr) {
        if (error != nullptr) *error = "llama.cpp failed to load the model";
        llama_backend_free();
        backend_initialized_ = false;
        return false;
    }
    return true;
#endif
}

bool ModelEngine::create_native_context(ContextSlot &slot, std::string *error) {
#ifndef AGENT_RUNTIME_WITH_LLAMA
    (void) slot;
    if (error != nullptr) *error = "llama.cpp support is disabled";
    return false;
#else
    llama_context_params parameters = llama_context_default_params();
    parameters.n_ctx = config_.context_tokens;
    parameters.n_batch = config_.batch_tokens;
    parameters.n_ubatch = config_.batch_tokens;
    const std::uint32_t cpu_threads = std::max(1U, config_.cpu_threads);
    parameters.n_threads = static_cast<std::int32_t>(cpu_threads);
    parameters.n_threads_batch = static_cast<std::int32_t>(cpu_threads);
    parameters.abort_callback = [](void *data) {
        return static_cast<ContextSlot *>(data)->cancel_requested.load(
            std::memory_order_relaxed);
    };
    parameters.abort_callback_data = &slot;

    slot.context = llama_init_from_model(model_, parameters);
    if (slot.context == nullptr) {
        if (error != nullptr) *error = "llama.cpp failed to create a context";
        return false;
    }
    return true;
#endif
}

std::optional<ContextId> ModelEngine::acquire(AgentId owner,
                                              std::string *error) {
    std::lock_guard lock(mutex_);
    if (model_ == nullptr) {
        if (error != nullptr) *error = "model engine is not initialized";
        return std::nullopt;
    }

    for (auto &[id, slot] : contexts_) {
        if (slot->context != nullptr && slot->owner == kInvalidAgentId) {
            slot->owner = owner;
            slot->cancel_requested.store(false);
            slot->last_active = std::chrono::steady_clock::now();
            return id;
        }
    }

    std::size_t resident_contexts = 0;
    for (const auto &[id, slot] : contexts_) {
        (void) id;
        if (slot->context != nullptr) ++resident_contexts;
    }
    if (resident_contexts >= config_.max_contexts) {
        if (error != nullptr) *error = "all model contexts are busy";
        return std::nullopt;
    }

    const auto memory = monitor_.sample(error);
    if (!memory.has_value()) {
        return std::nullopt;
    }

    const ContextId id = next_context_id_++;
    if (!admission_.try_reserve(id,
                                config_.context_reservation_bytes,
                                *memory,
                                error)) {
        return std::nullopt;
    }

    auto slot = std::make_unique<ContextSlot>();
    slot->id = id;
    slot->owner = owner;
    slot->last_active = std::chrono::steady_clock::now();
    if (!create_native_context(*slot, error)) {
        admission_.release(id);
        return std::nullopt;
    }
    contexts_.emplace(id, std::move(slot));
    return id;
}

bool ModelEngine::release(ContextId id, std::string *error) {
    std::lock_guard lock(mutex_);
    const auto it = contexts_.find(id);
    if (it == contexts_.end()) {
        if (error != nullptr) *error = "model context does not exist";
        return false;
    }
    it->second->owner = kInvalidAgentId;
    it->second->cancel_requested.store(false);
    it->second->last_active = std::chrono::steady_clock::now();
    return true;
}

bool ModelEngine::suspend(ContextId id,
                          const std::string &snapshot_path,
                          std::string *error) {
    std::lock_guard lock(mutex_);
    const auto it = contexts_.find(id);
    if (it == contexts_.end() || it->second->context == nullptr) {
        if (error != nullptr) *error = "resident model context does not exist";
        return false;
    }
#ifndef AGENT_RUNTIME_WITH_LLAMA
    (void) snapshot_path;
    if (error != nullptr) *error = "llama.cpp support is disabled";
    return false;
#else
    const std::size_t state_size = llama_state_get_size(it->second->context);
    std::vector<std::uint8_t> state(state_size);
    const std::size_t copied =
        llama_state_get_data(it->second->context, state.data(), state.size());
    if (copied == 0 || copied > state.size()) {
        if (error != nullptr) *error = "failed to serialize llama context";
        return false;
    }

    std::ofstream output(snapshot_path, std::ios::binary | std::ios::trunc);
    ContextSnapshotHeader header;
    header.state_size = copied;
    output.write(reinterpret_cast<const char *>(&header), sizeof(header));
    output.write(reinterpret_cast<const char *>(state.data()),
                 static_cast<std::streamsize>(copied));
    output.flush();
    if (!output) {
        if (error != nullptr) *error = "failed to write context snapshot";
        return false;
    }

    llama_free(it->second->context);
    it->second->context = nullptr;
    it->second->owner = kInvalidAgentId;
    it->second->snapshot_path = snapshot_path;
    admission_.release(id);
    return true;
#endif
}

bool ModelEngine::resume(ContextId id, std::string *error) {
    std::lock_guard lock(mutex_);
    const auto it = contexts_.find(id);
    if (it == contexts_.end() || it->second->context != nullptr ||
        it->second->snapshot_path.empty()) {
        if (error != nullptr) *error = "suspended model context does not exist";
        return false;
    }
#ifndef AGENT_RUNTIME_WITH_LLAMA
    if (error != nullptr) *error = "llama.cpp support is disabled";
    return false;
#else
    std::size_t resident_contexts = 0;
    for (const auto &[context_id, slot] : contexts_) {
        (void) context_id;
        if (slot->context != nullptr) ++resident_contexts;
    }
    if (resident_contexts >= config_.max_contexts) {
        if (error != nullptr) *error = "model context limit reached";
        return false;
    }
    const auto memory = monitor_.sample(error);
    if (!memory.has_value() ||
        !admission_.try_reserve(id,
                                config_.context_reservation_bytes,
                                *memory,
                                error)) {
        return false;
    }
    if (!create_native_context(*it->second, error)) {
        admission_.release(id);
        return false;
    }

    std::ifstream input(it->second->snapshot_path, std::ios::binary);
    ContextSnapshotHeader header;
    input.read(reinterpret_cast<char *>(&header), sizeof(header));
    std::error_code file_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(it->second->snapshot_path, file_error);
    if (!input || file_error || header.magic != kContextSnapshotMagic ||
        header.version != kContextSnapshotVersion ||
        header.state_size > std::numeric_limits<std::size_t>::max() ||
        file_size < sizeof(header) ||
        header.state_size != file_size - sizeof(header)) {
        llama_free(it->second->context);
        it->second->context = nullptr;
        admission_.release(id);
        if (error != nullptr) *error = "invalid context snapshot header";
        return false;
    }
    std::vector<std::uint8_t> state(
        static_cast<std::size_t>(header.state_size));
    input.read(reinterpret_cast<char *>(state.data()),
               static_cast<std::streamsize>(state.size()));
    const std::size_t restored = input
        ? llama_state_set_data(it->second->context, state.data(), state.size())
        : 0;
    if (!input || restored != state.size()) {
        llama_free(it->second->context);
        it->second->context = nullptr;
        admission_.release(id);
        if (error != nullptr) *error = "failed to restore context snapshot";
        return false;
    }
    it->second->cancel_requested.store(false);
    return true;
#endif
}

bool ModelEngine::request_cancel(ContextId id) {
    std::lock_guard lock(mutex_);
    const auto it = contexts_.find(id);
    if (it == contexts_.end()) {
        return false;
    }
    it->second->cancel_requested.store(true, std::memory_order_relaxed);
    return true;
}

llama_context *ModelEngine::native_context(ContextId id) noexcept {
    std::lock_guard lock(mutex_);
    const auto it = contexts_.find(id);
    return it == contexts_.end() ? nullptr : it->second->context;
}

bool ModelEngine::available() const noexcept {
    std::lock_guard lock(mutex_);
    return model_ != nullptr;
}

std::size_t ModelEngine::active_contexts() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto &[id, slot] : contexts_) {
        (void) id;
        if (slot->context != nullptr && slot->owner != kInvalidAgentId) ++count;
    }
    return count;
}

}  // namespace agent_runtime
