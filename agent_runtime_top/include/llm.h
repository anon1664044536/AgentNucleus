/**
 * @file llm.h
 * @brief C ABI for the local LLM backend used by agent_runtime_top.
 */

#ifndef AGENT_RUNTIME_TOP_LLM_H
#define AGENT_RUNTIME_TOP_LLM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LLM_MAX_PROMPT 8192
#define LLM_MAX_RESPONSE 4096
#define LLM_MAX_MODEL_PATH 256

typedef uint64_t llm_model_id_t;
typedef uint64_t llm_session_id_t;
typedef bool (*llm_cancel_callback)(void *user_data);

enum llm_kv_type {
    LLM_KV_TYPE_F16 = 0,
    LLM_KV_TYPE_Q8_0,
    LLM_KV_TYPE_Q4_0
};

enum llm_backend {
    LLM_BACKEND_LLAMA_CPP = 0,
    LLM_BACKEND_VLLM,
    LLM_BACKEND_OLLAMA,
    LLM_BACKEND_MAX
};

struct llm_model_config {
    char model_path[LLM_MAX_MODEL_PATH];
    int n_ctx;
    int n_batch;
    int n_seq_max;
    int n_threads;
    int n_gpu_layers;
    float temperature;
    float top_p;
    int max_tokens;
    bool use_mmap;
    bool use_mlock;
    enum llm_kv_type kv_type_k;
    enum llm_kv_type kv_type_v;
};

struct llm_request {
    llm_session_id_t session;
    const char *prompt;
    const char *system_prompt;
    int seq_id;
    bool is_first_turn;
    /**
     * Persist an end-of-turn token in the private sequence. The token is not
     * returned as visible text and is not counted against max_tokens.
     */
    bool close_turn;
    int cpu_threads;
    float temperature;
    float top_p;
    int max_tokens;
    bool stream;
    llm_cancel_callback cancel_callback;
    void *cancel_user_data;
};

struct llm_response {
    char *text;
    int tokens_generated;
    int prompt_tokens;
    int total_tokens;
    int seq_id;
    float eval_time_ms;
    bool stopped;
    bool cancelled;
    bool cache_hit;
    int reused_tokens;
    uint64_t cache_bytes;
};

struct llm_stats {
    uint64_t total_requests;
    uint64_t total_tokens;
    uint64_t prefix_evictions;
    float avg_latency_ms;
    float tokens_per_second;
};

struct llm_chat_message_value {
    const char *role;
    const char *content;
};

int llm_manager_init(void);
void llm_manager_destroy(void);

int llm_load_model(enum llm_backend backend,
                   const struct llm_model_config *config,
                   llm_model_id_t *model_id);
void llm_unload_model(llm_model_id_t model_id);

int llm_create_session(llm_model_id_t model_id,
                       llm_session_id_t *session_id);
void llm_destroy_session(llm_session_id_t session_id);

/**
 * Runs one turn on an Agent-private llama sequence.
 *
 * Returns 0 on success, -2 on cancellation, and -1 on validation,
 * allocation, tokenization, context-capacity, or decode failure.
 * The caller must always pass the response to llm_free_response().
 */
int llm_inference(llm_model_id_t model_id,
                  const struct llm_request *request,
                  struct llm_response *response);
void llm_free_response(struct llm_response *response);
int llm_apply_chat_template(
    llm_model_id_t model_id,
    const struct llm_chat_message_value *messages,
    uint32_t message_count,
    bool add_assistant_prompt,
    char **formatted_text,
    uint32_t *formatted_length);
void llm_free_text(char *text);

int llm_get_model_info(llm_model_id_t model_id,
                       char *info,
                       uint32_t info_size);
int llm_get_stats(llm_model_id_t model_id, struct llm_stats *stats);

/**
 * Encodes an immutable system prompt into a reserved llama sequence.
 * Returns its reserved sequence id, or -1 on failure/cache exhaustion.
 */
int llm_warmup_shared_kv(llm_model_id_t model_id,
                         const char *system_prompt);
int llm_warmup_shared_kv_with_cancel(
    llm_model_id_t model_id,
    const char *system_prompt,
    llm_cancel_callback cancel_callback,
    void *cancel_user_data);
void llm_get_shared_kv_stats(int *total_shares, int *saved_tokens);

/**
 * Clears only a private Agent sequence. Reserved shared-prefix sequences
 * cannot be removed through this API.
 */
void llm_clear_seq(llm_model_id_t model_id, int seq_id);

int llm_checkpoint_seq(llm_model_id_t model_id,
                       int seq_id,
                       const char *path,
                       int *position,
                       uint64_t *bytes_written);
int llm_restore_seq(llm_model_id_t model_id,
                    int seq_id,
                    const char *path,
                    int position,
                    uint64_t *bytes_read);

#ifdef __cplusplus
}
#endif

#endif
