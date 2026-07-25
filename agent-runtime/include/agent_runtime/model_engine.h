#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "agent_runtime/resource_monitor.h"
#include "agent_runtime/types.h"

struct llama_context;
struct llama_model;

namespace agent_runtime {

struct ModelEngineConfig {
    std::string model_path;
    std::uint32_t context_tokens{4096};
    std::uint32_t batch_tokens{512};
    std::uint32_t cpu_threads{4};
    std::size_t max_contexts{2};
    std::uint64_t model_reservation_bytes{0};
    std::uint64_t context_reservation_bytes{512ULL * 1024ULL * 1024ULL};
    std::uint64_t safety_margin_bytes{512ULL * 1024ULL * 1024ULL};
};

class ModelEngine {
public:
    explicit ModelEngine(ModelEngineConfig config);
    ~ModelEngine();

    ModelEngine(const ModelEngine &) = delete;
    ModelEngine &operator=(const ModelEngine &) = delete;

    bool initialize(std::string *error = nullptr);
    std::optional<ContextId> acquire(AgentId owner,
                                     std::string *error = nullptr);
    bool release(ContextId id, std::string *error = nullptr);
    bool suspend(ContextId id,
                 const std::string &snapshot_path,
                 std::string *error = nullptr);
    bool resume(ContextId id, std::string *error = nullptr);
    bool request_cancel(ContextId id);
    llama_context *native_context(ContextId id) noexcept;
    bool available() const noexcept;
    std::size_t active_contexts() const;

private:
    struct ContextSlot;

    bool create_native_context(ContextSlot &slot, std::string *error);

    ModelEngineConfig config_;
    ResourceMonitor monitor_;
    MemoryAdmissionController admission_;
    mutable std::mutex mutex_;
    llama_model *model_{nullptr};
    std::unordered_map<ContextId, std::unique_ptr<ContextSlot>> contexts_;
    ContextId next_context_id_{1};
    bool backend_initialized_{false};
};

}  // namespace agent_runtime
