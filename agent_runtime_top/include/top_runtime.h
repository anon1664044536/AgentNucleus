#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agent_runtime/invocation_channel.h"
#include "context_manager.h"
#include "llm.h"
#include "tool_manager.h"

namespace agent_runtime_top {

struct TopRuntimeConfig {
    std::string model_path;
    std::string model_alias{"default"};
    std::uint32_t context_tokens{4096};
    std::uint32_t batch_tokens{512};
    std::uint32_t cpu_threads{4};
    std::size_t max_contexts{16};
    std::size_t max_swapped_contexts{0};
    std::uint32_t max_tokens{256};
    float temperature{0.7F};
    float top_p{0.9F};
    int gpu_layers{0};
    llm_kv_type kv_type_k{LLM_KV_TYPE_F16};
    llm_kv_type kv_type_v{LLM_KV_TYPE_F16};
    bool use_mmap{true};
    bool use_mlock{false};
    bool enable_shell_tool{false};
    bool use_chat_template{true};
    bool enable_context_swap{false};
    std::string swap_directory;
};

class TopRuntime {
public:
    explicit TopRuntime(TopRuntimeConfig config);
    ~TopRuntime();

    TopRuntime(const TopRuntime &) = delete;
    TopRuntime &operator=(const TopRuntime &) = delete;

    bool initialize(std::string *error = nullptr);
    void shutdown();
    agent_runtime::InvocationHandlerResult handle(
        const agent_runtime::InvocationRequest &request,
        const std::vector<agent_runtime::InvocationMappedInput> &inputs,
        const std::function<bool()> &cancel_requested);

private:
    agent_runtime::InvocationHandlerResult handle_model(
        const agent_runtime::InvocationRequest &request,
        const std::vector<agent_runtime::InvocationMappedInput> &inputs,
        const std::function<bool()> &cancel_requested);
    agent_runtime::InvocationHandlerResult handle_tool(
        const agent_runtime::InvocationRequest &request,
        const std::vector<agent_runtime::InvocationMappedInput> &inputs,
        const std::function<bool()> &cancel_requested);
    agent_runtime::InvocationHandlerResult make_text_result(
        std::string text,
        agent_runtime::SharedDataType type,
        std::string *error = nullptr) const;
    bool initialize_snapshot_store(std::string *error);
    bool swap_out_context(ContextId context_id,
                          int sequence_id,
                          std::string *error);
    bool swap_in_context(ContextId context_id,
                         int sequence_id,
                         std::string *error);
    bool format_chat(
        const std::vector<std::pair<std::string, std::string>> &messages,
        bool add_assistant_prompt,
        std::string *formatted) const;
    void remove_snapshot(ContextId context_id);
    void cleanup_snapshots();

    struct Snapshot {
        std::string path;
        int position{0};
        std::uint64_t bytes{0};
    };

    struct Conversation {
        std::vector<std::pair<std::string, std::string>> messages;
        bool template_active{true};
    };

    TopRuntimeConfig config_;
    ContextManager contexts_;
    ToolManager tools_;
    llm_model_id_t model_id_{0};
    bool initialized_{false};
    std::shared_mutex lifecycle_mutex_;
    std::string active_swap_directory_;
    bool owns_swap_directory_{false};
    std::unordered_map<ContextId, Snapshot> snapshots_;
    std::uint64_t snapshot_bytes_{0};
    std::mutex snapshot_mutex_;
    std::unordered_map<ContextId, Conversation> conversations_;
    std::mutex conversation_mutex_;
};

}  // namespace agent_runtime_top
