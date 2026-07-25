/**
 * @file llm_llamacpp.c
 * @brief llama.cpp adapter with per-Agent sequence isolation and shared prefixes.
 */

#include "llm.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef USE_LLAMA_CPP
#include "llama.h"
#endif

#define MAX_MODELS 4
#define MAX_SESSIONS 64
#define MAX_SHARED_SYS 8
#define MAX_SEQ_PER_MODEL 64

typedef struct {
    int seq_id;
    int token_count;
    char *prompt_text;
    int share_count;
    int saved_tokens;
    uint64_t last_access;
} shared_sys_entry_t;

typedef struct {
    int current_pos;
    bool in_use;
} seq_state_t;

struct llm_model {
    llm_model_id_t id;
    bool used;
    struct llm_model_config config;
#ifdef USE_LLAMA_CPP
    struct llama_model *model;
    const struct llama_vocab *vocab;
    struct llama_context *ctx;
#endif
    struct llm_stats stats;
    double total_eval_ms;
    pthread_mutex_t inference_lock;
    shared_sys_entry_t shared_sys[MAX_SHARED_SYS];
    int shared_sys_count;
    uint64_t prefix_clock;
};

struct llm_session {
    llm_session_id_t id;
    llm_model_id_t model_id;
    bool used;
};

static struct {
    struct llm_model models[MAX_MODELS];
    struct llm_session sessions[MAX_SESSIONS];
    seq_state_t seq_states[MAX_MODELS][MAX_SEQ_PER_MODEL];
    uint32_t model_count;
    uint32_t session_count;
    llm_model_id_t next_model_id;
    llm_session_id_t next_session_id;
    pthread_mutex_t lock;
    bool initialized;
} g_llm_manager;

static uint64_t monotonic_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static struct llm_model *find_model(llm_model_id_t model_id) {
    for (int i = 0; i < MAX_MODELS; ++i) {
        if (g_llm_manager.models[i].used &&
            g_llm_manager.models[i].id == model_id) {
            return &g_llm_manager.models[i];
        }
    }
    return NULL;
}

static int model_index(const struct llm_model *model) {
    for (int i = 0; i < MAX_MODELS; ++i) {
        if (&g_llm_manager.models[i] == model) {
            return i;
        }
    }
    return -1;
}

static int find_shared_sys(const struct llm_model *model,
                           const char *prompt) {
    for (int i = 0; i < MAX_SHARED_SYS; ++i) {
        if (model->shared_sys[i].prompt_text &&
            strcmp(model->shared_sys[i].prompt_text, prompt) == 0) {
            return i;
        }
    }
    return -1;
}

static void touch_shared_sys(struct llm_model *model, int index) {
    model->shared_sys[index].last_access = ++model->prefix_clock;
}

static int select_shared_sys_slot(const struct llm_model *model) {
    int lru_index = 0;
    for (int i = 0; i < MAX_SHARED_SYS; ++i) {
        if (!model->shared_sys[i].prompt_text) {
            return i;
        }
        if (model->shared_sys[i].last_access <
            model->shared_sys[lru_index].last_access) {
            lru_index = i;
        }
    }
    return lru_index;
}

static int private_seq_limit(const struct llm_model *model) {
    return model->config.n_seq_max - MAX_SHARED_SYS;
}

static bool request_cancelled(const struct llm_request *request) {
    return request && request->cancel_callback &&
           request->cancel_callback(request->cancel_user_data);
}

static void update_stats(struct llm_model *model,
                         int generated_tokens,
                         double elapsed_ms) {
    model->stats.total_requests++;
    model->stats.total_tokens += (uint64_t)generated_tokens;
    model->total_eval_ms += elapsed_ms;
    model->stats.avg_latency_ms =
        (float)(model->total_eval_ms / (double)model->stats.total_requests);
    model->stats.tokens_per_second =
        model->total_eval_ms > 0.0
            ? (float)((double)model->stats.total_tokens * 1000.0 /
                      model->total_eval_ms)
            : 0.0f;
}

#ifdef USE_LLAMA_CPP

static enum ggml_type map_kv_type(enum llm_kv_type type) {
    switch (type) {
        case LLM_KV_TYPE_Q8_0:
            return GGML_TYPE_Q8_0;
        case LLM_KV_TYPE_Q4_0:
            return GGML_TYPE_Q4_0;
        case LLM_KV_TYPE_F16:
        default:
            return GGML_TYPE_F16;
    }
}

static int tokenize_alloc(const struct llm_model *model,
                          const char *text,
                          bool add_special,
                          llama_token **tokens_out) {
    if (!text || !tokens_out) {
        return -1;
    }

    *tokens_out = NULL;
    int32_t count = llama_tokenize(model->vocab, text,
                                   (int32_t)strlen(text), NULL, 0,
                                   add_special, false);
    if (count == INT32_MIN) {
        return -1;
    }
    if (count < 0) {
        count = -count;
    }
    if (count == 0) {
        return 0;
    }

    llama_token *tokens =
        (llama_token *)malloc(sizeof(llama_token) * (size_t)count);
    if (!tokens) {
        return -1;
    }
    int32_t actual = llama_tokenize(model->vocab, text,
                                    (int32_t)strlen(text), tokens, count,
                                    add_special, false);
    if (actual < 0) {
        free(tokens);
        return -1;
    }

    *tokens_out = tokens;
    return actual;
}

static int decode_tokens(struct llm_model *model,
                         const llama_token *tokens,
                         int token_count,
                         int seq_id,
                         int start_pos,
                         bool need_final_logits,
                         const struct llm_request *request) {
    int offset = 0;
    while (offset < token_count) {
        if (request && request_cancelled(request)) {
            return -2;
        }

        int chunk = token_count - offset;
        if (chunk > model->config.n_batch) {
            chunk = model->config.n_batch;
        }

        struct llama_batch batch = llama_batch_init(chunk, 0, 1);
        batch.n_tokens = chunk;
        for (int i = 0; i < chunk; ++i) {
            batch.token[i] = tokens[offset + i];
            batch.pos[i] = start_pos + offset + i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = seq_id;
            batch.logits[i] = 0;
        }
        if (need_final_logits && offset + chunk == token_count) {
            batch.logits[chunk - 1] = 1;
        }

        int result = llama_decode(model->ctx, batch);
        llama_batch_free(batch);
        if (result != 0) {
            return request_cancelled(request) ? -2 : -1;
        }
        offset += chunk;
    }
    return 0;
}

static int append_token_piece(const struct llm_model *model,
                              llama_token token,
                              char **text,
                              size_t *length,
                              size_t *capacity) {
    char local[256];
    char *piece = local;
    int32_t piece_capacity = (int32_t)sizeof(local);
    int32_t count = llama_token_to_piece(model->vocab, token, piece,
                                         piece_capacity, 0, true);
    if (count < 0) {
        piece_capacity = -count;
        piece = (char *)malloc((size_t)piece_capacity);
        if (!piece) {
            return -1;
        }
        count = llama_token_to_piece(model->vocab, token, piece,
                                     piece_capacity, 0, true);
    }
    if (count < 0) {
        if (piece != local) {
            free(piece);
        }
        return -1;
    }

    size_t required = *length + (size_t)count + 1;
    if (required > *capacity) {
        size_t new_capacity = *capacity;
        while (new_capacity < required) {
            new_capacity *= 2;
        }
        char *grown = (char *)realloc(*text, new_capacity);
        if (!grown) {
            if (piece != local) {
                free(piece);
            }
            return -1;
        }
        *text = grown;
        *capacity = new_capacity;
    }

    memcpy(*text + *length, piece, (size_t)count);
    *length += (size_t)count;
    (*text)[*length] = '\0';
    if (piece != local) {
        free(piece);
    }
    return 0;
}

static int cache_system_prefix(struct llm_model *model,
                               const char *system_prompt,
                               int *entry_index) {
    llama_token *tokens = NULL;
    int count = tokenize_alloc(model, system_prompt, true, &tokens);
    if (count <= 0 || count >= model->config.n_ctx) {
        free(tokens);
        return -1;
    }

    int index = select_shared_sys_slot(model);
    int shared_seq_id = model->config.n_seq_max - 1 - index;
    llama_memory_t memory = llama_get_memory(model->ctx);
    shared_sys_entry_t *entry = &model->shared_sys[index];
    if (entry->prompt_text) {
        llama_memory_seq_rm(memory, entry->seq_id, -1, -1);
        free(entry->prompt_text);
        memset(entry, 0, sizeof(*entry));
        model->shared_sys_count--;
        model->stats.prefix_evictions++;
    }
    llama_memory_seq_rm(memory, shared_seq_id, -1, -1);

    int decoded = decode_tokens(model, tokens, count, shared_seq_id, 0,
                                false, NULL);
    free(tokens);
    if (decoded != 0) {
        llama_memory_seq_rm(memory, shared_seq_id, -1, -1);
        return -1;
    }

    char *copy = strdup(system_prompt);
    if (!copy) {
        llama_memory_seq_rm(memory, shared_seq_id, -1, -1);
        return -1;
    }

    entry->seq_id = shared_seq_id;
    entry->token_count = count;
    entry->prompt_text = copy;
    entry->share_count = 0;
    entry->saved_tokens = 0;
    touch_shared_sys(model, index);
    model->shared_sys_count++;
    if (entry_index) {
        *entry_index = index;
    }
    return 0;
}

#endif

int llm_manager_init(void) {
    if (g_llm_manager.initialized) {
        return -1;
    }

    memset(&g_llm_manager, 0, sizeof(g_llm_manager));
    if (pthread_mutex_init(&g_llm_manager.lock, NULL) != 0) {
        return -1;
    }
    g_llm_manager.next_model_id = 1;
    g_llm_manager.next_session_id = 1;
    g_llm_manager.initialized = true;
#ifdef USE_LLAMA_CPP
    llama_backend_init();
#endif
    return 0;
}

void llm_manager_destroy(void) {
    if (!g_llm_manager.initialized) {
        return;
    }

    for (int i = 0; i < MAX_MODELS; ++i) {
        if (g_llm_manager.models[i].used) {
            llm_unload_model(g_llm_manager.models[i].id);
        }
    }

    pthread_mutex_destroy(&g_llm_manager.lock);
#ifdef USE_LLAMA_CPP
    llama_backend_free();
#endif
    memset(&g_llm_manager, 0, sizeof(g_llm_manager));
}

int llm_load_model(enum llm_backend backend,
                   const struct llm_model_config *config,
                   llm_model_id_t *model_id) {
    if (!config || !model_id || !g_llm_manager.initialized ||
        backend != LLM_BACKEND_LLAMA_CPP) {
        return -1;
    }

    pthread_mutex_lock(&g_llm_manager.lock);
    int slot = -1;
    for (int i = 0; i < MAX_MODELS; ++i) {
        if (!g_llm_manager.models[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }

    struct llm_model *model = &g_llm_manager.models[slot];
    memset(model, 0, sizeof(*model));
    model->id = g_llm_manager.next_model_id++;
    model->used = true;
    model->config = *config;
    if (model->config.n_ctx <= 0) {
        model->config.n_ctx = 4096;
    }
    if (model->config.n_batch <= 0) {
        model->config.n_batch = 512;
    }
    if (model->config.n_seq_max <= MAX_SHARED_SYS + 1) {
        model->config.n_seq_max = 32;
    }
    if (model->config.n_seq_max > MAX_SEQ_PER_MODEL) {
        model->used = false;
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }
    if (model->config.n_threads <= 0) {
        model->config.n_threads = 4;
    }
    if (model->config.max_tokens <= 0) {
        model->config.max_tokens = 256;
    }
    if (model->config.temperature <= 0.0f) {
        model->config.temperature = 0.7f;
    }
    if (model->config.top_p <= 0.0f ||
        model->config.top_p > 1.0f) {
        model->config.top_p = 0.9f;
    }
    if (pthread_mutex_init(&model->inference_lock, NULL) != 0) {
        model->used = false;
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }

#ifdef USE_LLAMA_CPP
    struct llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = model->config.n_gpu_layers;
    model_params.use_mmap = model->config.use_mmap;
    model_params.use_mlock = model->config.use_mlock;
    model->model =
        llama_model_load_from_file(model->config.model_path, model_params);
    if (!model->model) {
        model->used = false;
        pthread_mutex_destroy(&model->inference_lock);
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }

    model->vocab = llama_model_get_vocab(model->model);
    if (!model->vocab) {
        llama_model_free(model->model);
        model->model = NULL;
        model->used = false;
        pthread_mutex_destroy(&model->inference_lock);
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }

    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = (uint32_t)model->config.n_ctx;
    ctx_params.n_batch = (uint32_t)model->config.n_batch;
    ctx_params.n_ubatch = (uint32_t)model->config.n_batch;
    ctx_params.n_seq_max = (uint32_t)model->config.n_seq_max;
    ctx_params.n_threads = model->config.n_threads;
    ctx_params.n_threads_batch = model->config.n_threads;
    ctx_params.type_k = map_kv_type(model->config.kv_type_k);
    ctx_params.type_v = map_kv_type(model->config.kv_type_v);
    ctx_params.kv_unified = true;

    model->ctx = llama_init_from_model(model->model, ctx_params);
    if (!model->ctx) {
        llama_model_free(model->model);
        model->model = NULL;
        model->used = false;
        pthread_mutex_destroy(&model->inference_lock);
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }
#endif

    memset(g_llm_manager.seq_states[slot], 0,
           sizeof(g_llm_manager.seq_states[slot]));
    g_llm_manager.model_count++;
    *model_id = model->id;
    pthread_mutex_unlock(&g_llm_manager.lock);
    return 0;
}

void llm_unload_model(llm_model_id_t model_id) {
    if (!g_llm_manager.initialized) {
        return;
    }

    pthread_mutex_lock(&g_llm_manager.lock);
    struct llm_model *model = find_model(model_id);
    if (!model) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return;
    }

    pthread_mutex_lock(&model->inference_lock);
    for (int i = 0; i < MAX_SHARED_SYS; ++i) {
        free(model->shared_sys[i].prompt_text);
        model->shared_sys[i].prompt_text = NULL;
    }
#ifdef USE_LLAMA_CPP
    if (model->ctx) {
        llama_free(model->ctx);
        model->ctx = NULL;
    }
    if (model->model) {
        llama_model_free(model->model);
        model->model = NULL;
    }
#endif
    model->used = false;
    g_llm_manager.model_count--;
    pthread_mutex_unlock(&model->inference_lock);
    pthread_mutex_destroy(&model->inference_lock);
    pthread_mutex_unlock(&g_llm_manager.lock);
}

int llm_create_session(llm_model_id_t model_id,
                       llm_session_id_t *session_id) {
    if (!session_id || !g_llm_manager.initialized) {
        return -1;
    }

    pthread_mutex_lock(&g_llm_manager.lock);
    if (!find_model(model_id)) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }

    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (!g_llm_manager.sessions[i].used) {
            struct llm_session *session = &g_llm_manager.sessions[i];
            session->id = g_llm_manager.next_session_id++;
            session->model_id = model_id;
            session->used = true;
            g_llm_manager.session_count++;
            *session_id = session->id;
            pthread_mutex_unlock(&g_llm_manager.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_llm_manager.lock);
    return -1;
}

void llm_destroy_session(llm_session_id_t session_id) {
    if (!g_llm_manager.initialized) {
        return;
    }

    pthread_mutex_lock(&g_llm_manager.lock);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (g_llm_manager.sessions[i].used &&
            g_llm_manager.sessions[i].id == session_id) {
            g_llm_manager.sessions[i].used = false;
            g_llm_manager.session_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_llm_manager.lock);
}

int llm_inference(llm_model_id_t model_id,
                  const struct llm_request *request,
                  struct llm_response *response) {
    if (!request || !request->prompt || !response ||
        !g_llm_manager.initialized) {
        return -1;
    }
    memset(response, 0, sizeof(*response));

    pthread_mutex_lock(&g_llm_manager.lock);
    struct llm_model *model = find_model(model_id);
    if (!model) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }

    int index = model_index(model);
    int seq_id = request->seq_id;
    if (seq_id <= 0 || seq_id >= private_seq_limit(model) ||
        index < 0) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }

    pthread_mutex_lock(&model->inference_lock);
    pthread_mutex_unlock(&g_llm_manager.lock);

    seq_state_t *state = &g_llm_manager.seq_states[index][seq_id];
    int original_pos = state->current_pos;
    bool original_in_use = state->in_use;
    int max_tokens = request->max_tokens > 0
                         ? request->max_tokens
                         : model->config.max_tokens;
#ifdef USE_LLAMA_CPP
    float temperature = request->temperature > 0.0f
                            ? request->temperature
                            : model->config.temperature;
    float top_p = request->top_p > 0.0f
                      ? request->top_p
                      : model->config.top_p;
#endif
    uint64_t started = monotonic_us();
    int result = -1;

    if (max_tokens > model->config.n_ctx) {
        max_tokens = model->config.n_ctx;
    }
    response->seq_id = seq_id;
    if (request_cancelled(request)) {
        response->cancelled = true;
        pthread_mutex_unlock(&model->inference_lock);
        return -2;
    }

#ifdef USE_LLAMA_CPP
    llama_memory_t memory = llama_get_memory(model->ctx);
    int request_threads = request->cpu_threads > 0
                              ? request->cpu_threads
                              : model->config.n_threads;
    if (request_threads > model->config.n_threads) {
        request_threads = model->config.n_threads;
    }
    llama_set_n_threads(model->ctx, request_threads, request_threads);
    llama_set_abort_callback(model->ctx, request->cancel_callback,
                             request->cancel_user_data);

    if (request->is_first_turn) {
        llama_memory_seq_rm(memory, seq_id, -1, -1);
        state->current_pos = 0;
        state->in_use = false;
        original_pos = 0;
        original_in_use = false;
    }

    if (request->is_first_turn && request->system_prompt &&
        request->system_prompt[0] != '\0') {
        int shared_index = find_shared_sys(model, request->system_prompt);
        bool reused_cached_prefix = shared_index >= 0;
        if (shared_index < 0) {
            cache_system_prefix(model, request->system_prompt,
                                &shared_index);
        }

        if (shared_index >= 0) {
            shared_sys_entry_t *entry = &model->shared_sys[shared_index];
            touch_shared_sys(model, shared_index);
            llama_memory_seq_cp(memory, entry->seq_id, seq_id,
                                0, entry->token_count);
            state->current_pos = entry->token_count;
            state->in_use = true;
            if (reused_cached_prefix) {
                entry->share_count++;
                entry->saved_tokens += entry->token_count;
                response->cache_hit = true;
                response->reused_tokens = entry->token_count;
            }
        } else {
            llama_token *system_tokens = NULL;
            int system_count =
                tokenize_alloc(model, request->system_prompt, true,
                               &system_tokens);
            if (system_count <= 0 ||
                system_count >= model->config.n_ctx ||
                decode_tokens(model, system_tokens, system_count, seq_id,
                              0, false, request) != 0) {
                free(system_tokens);
                response->cancelled = request_cancelled(request);
                goto native_cleanup;
            }
            free(system_tokens);
            state->current_pos = system_count;
            state->in_use = true;
        }
    }

    {
        llama_token *prompt_tokens = NULL;
        int prompt_count =
            tokenize_alloc(model, request->prompt, !state->in_use,
                           &prompt_tokens);
        const int boundary_reserve = request->close_turn ? 1 : 0;
        if (prompt_count <= 0 ||
            state->current_pos + prompt_count + max_tokens +
                    boundary_reserve >
                model->config.n_ctx) {
            free(prompt_tokens);
            goto native_cleanup;
        }

        int decode_result =
            decode_tokens(model, prompt_tokens, prompt_count, seq_id,
                          state->current_pos, true, request);
        free(prompt_tokens);
        if (decode_result != 0) {
            response->cancelled =
                decode_result == -2 || request_cancelled(request);
            goto native_cleanup;
        }

        int generation_pos = state->current_pos + prompt_count;
        size_t text_capacity = 256;
        size_t text_length = 0;
        response->text = (char *)calloc(text_capacity, 1);
        if (!response->text) {
            goto native_cleanup;
        }

        struct llama_sampler_chain_params chain_params =
            llama_sampler_chain_default_params();
        struct llama_sampler *sampler =
            llama_sampler_chain_init(chain_params);
        if (!sampler) {
            goto native_cleanup;
        }
        llama_sampler_chain_add(sampler, llama_sampler_init_top_k(50));
        llama_sampler_chain_add(sampler,
                                llama_sampler_init_top_p(top_p, 1));
        llama_sampler_chain_add(
            sampler, llama_sampler_init_penalties(64, 1.1f, 0.0f, 0.0f));
        llama_sampler_chain_add(sampler,
                                llama_sampler_init_temp(temperature));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(0));

        int generated = 0;
        int boundary_tokens = 0;
        int generation_result = 0;
        while (generated < max_tokens) {
            if (request_cancelled(request)) {
                response->cancelled = true;
                generation_result = -2;
                break;
            }

            llama_token token =
                llama_sampler_sample(sampler, model->ctx, -1);
            if (llama_vocab_is_eog(model->vocab, token)) {
                const int token_result =
                    decode_tokens(model, &token, 1, seq_id,
                                  generation_pos + generated, true,
                                  request);
                if (token_result != 0) {
                    response->cancelled =
                        token_result == -2 ||
                        request_cancelled(request);
                    generation_result =
                        response->cancelled ? -2 : -1;
                    break;
                }
                boundary_tokens = 1;
                response->stopped = true;
                break;
            }
            if (append_token_piece(model, token, &response->text,
                                   &text_length, &text_capacity) != 0) {
                generation_result = -1;
                break;
            }

            int token_result =
                decode_tokens(model, &token, 1, seq_id,
                              generation_pos + generated, true, request);
            if (token_result != 0) {
                response->cancelled =
                    token_result == -2 || request_cancelled(request);
                generation_result = response->cancelled ? -2 : -1;
                break;
            }
            ++generated;
        }

        if (generation_result == 0 && request->close_turn &&
            boundary_tokens == 0) {
            llama_token boundary = llama_vocab_eot(model->vocab);
            if (boundary == LLAMA_TOKEN_NULL) {
                boundary = llama_vocab_eos(model->vocab);
            }
            if (boundary != LLAMA_TOKEN_NULL) {
                const int boundary_result =
                    decode_tokens(model, &boundary, 1, seq_id,
                                  generation_pos + generated, true,
                                  request);
                if (boundary_result != 0) {
                    response->cancelled =
                        boundary_result == -2 ||
                        request_cancelled(request);
                    generation_result =
                        response->cancelled ? -2 : -1;
                } else {
                    boundary_tokens = 1;
                    response->stopped = true;
                }
            }
        }
        llama_sampler_free(sampler);

        if (generation_result != 0) {
            result = generation_result;
            goto native_cleanup;
        }

        response->tokens_generated = generated;
        response->prompt_tokens = prompt_count;
        response->total_tokens =
            generation_pos + response->tokens_generated +
            boundary_tokens;
        state->current_pos = response->total_tokens;
        state->in_use = true;
        response->cache_bytes =
            (uint64_t)llama_state_seq_get_size(model->ctx, seq_id);
        result = response->cancelled ? -2 : 0;
    }

native_cleanup:
    llama_set_abort_callback(model->ctx, NULL, NULL);
    if (result != 0) {
        const bool rolled_back =
            llama_memory_seq_rm(memory, seq_id, original_pos, -1);
        if (!rolled_back || !original_in_use) {
            llama_memory_seq_rm(memory, seq_id, -1, -1);
            state->current_pos = 0;
            state->in_use = false;
        } else {
            state->current_pos = original_pos;
            state->in_use = true;
        }
        if (!response->cancelled) {
            llm_free_response(response);
        }
    }
#else
    if (request->is_first_turn) {
        state->current_pos = 0;
        state->in_use = false;
    }
    if (request->is_first_turn && request->system_prompt &&
        request->system_prompt[0] != '\0') {
        int shared_index = find_shared_sys(model, request->system_prompt);
        const bool reused_cached_prefix = shared_index >= 0;
        if (shared_index < 0) {
            char *prompt_copy = strdup(request->system_prompt);
            if (!prompt_copy) {
                state->current_pos = original_pos;
                state->in_use = original_in_use;
                pthread_mutex_unlock(&model->inference_lock);
                return -1;
            }
            shared_index = select_shared_sys_slot(model);
            shared_sys_entry_t *entry = &model->shared_sys[shared_index];
            if (entry->prompt_text) {
                free(entry->prompt_text);
                memset(entry, 0, sizeof(*entry));
                model->shared_sys_count--;
                model->stats.prefix_evictions++;
            }
            entry->seq_id = model->config.n_seq_max - 1 - shared_index;
            entry->token_count =
                (int)strlen(request->system_prompt) / 4 + 1;
            entry->prompt_text = prompt_copy;
            touch_shared_sys(model, shared_index);
            model->shared_sys_count++;
        }
        if (shared_index >= 0) {
            shared_sys_entry_t *entry = &model->shared_sys[shared_index];
            touch_shared_sys(model, shared_index);
            if (reused_cached_prefix) {
                entry->share_count++;
                entry->saved_tokens += entry->token_count;
                response->cache_hit = true;
                response->reused_tokens = entry->token_count;
            }
            state->current_pos = entry->token_count;
        }
    }

    response->text = (char *)calloc(LLM_MAX_RESPONSE, 1);
    if (response->text) {
        snprintf(response->text, LLM_MAX_RESPONSE,
                 "[simulation model=%" PRIu64 " seq=%d] %.4000s",
                 (uint64_t)model_id, seq_id, request->prompt);
        response->prompt_tokens = (int)strlen(request->prompt) / 4 + 1;
        response->tokens_generated = 10;
        response->total_tokens =
            state->current_pos + response->prompt_tokens +
            response->tokens_generated +
            (request->close_turn ? 1 : 0);
        state->current_pos = response->total_tokens;
        state->in_use = true;
        response->stopped = true;
        response->cache_bytes =
            (uint64_t)response->total_tokens * 1024ULL;
        result = 0;
    }
#endif

    response->eval_time_ms =
        (float)((double)(monotonic_us() - started) / 1000.0);
    if (result == 0) {
        update_stats(model, response->tokens_generated,
                     response->eval_time_ms);
    }
    pthread_mutex_unlock(&model->inference_lock);
    return result;
}

void llm_free_response(struct llm_response *response) {
    if (!response) {
        return;
    }
    free(response->text);
    response->text = NULL;
}

int llm_apply_chat_template(
    llm_model_id_t model_id,
    const struct llm_chat_message_value *messages,
    uint32_t message_count,
    bool add_assistant_prompt,
    char **formatted_text,
    uint32_t *formatted_length) {
    if (!messages || message_count == 0 || !formatted_text ||
        !formatted_length || !g_llm_manager.initialized) {
        return -1;
    }
    *formatted_text = NULL;
    *formatted_length = 0;
    for (uint32_t i = 0; i < message_count; ++i) {
        if (!messages[i].role || !messages[i].content) return -1;
    }

    pthread_mutex_lock(&g_llm_manager.lock);
    struct llm_model *model = find_model(model_id);
    if (!model) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }
    pthread_mutex_lock(&model->inference_lock);
    pthread_mutex_unlock(&g_llm_manager.lock);

#ifdef USE_LLAMA_CPP
    const char *chat_template =
        llama_model_chat_template(model->model, NULL);
    if (!chat_template) {
        pthread_mutex_unlock(&model->inference_lock);
        return -2;
    }
    struct llama_chat_message *native_messages =
        (struct llama_chat_message *)calloc(
            message_count, sizeof(struct llama_chat_message));
    if (!native_messages) {
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    for (uint32_t i = 0; i < message_count; ++i) {
        native_messages[i].role = messages[i].role;
        native_messages[i].content = messages[i].content;
    }
    int32_t required = llama_chat_apply_template(
        chat_template,
        native_messages,
        message_count,
        add_assistant_prompt,
        NULL,
        0);
    if (required < 0 || required == INT32_MAX) {
        free(native_messages);
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    char *output = (char *)malloc((size_t)required + 1);
    if (!output) {
        free(native_messages);
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    const int32_t actual = llama_chat_apply_template(
        chat_template,
        native_messages,
        message_count,
        add_assistant_prompt,
        output,
        required);
    free(native_messages);
    if (actual < 0 || actual > required) {
        free(output);
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    output[actual] = '\0';
    *formatted_text = output;
    *formatted_length = (uint32_t)actual;
#else
    size_t required = add_assistant_prompt ? 16U : 0U;
    for (uint32_t i = 0; i < message_count; ++i) {
        required += strlen(messages[i].role) +
                    strlen(messages[i].content) + 8U;
    }
    if (required > UINT32_MAX) {
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    char *output = (char *)calloc(required + 1, 1);
    if (!output) {
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    size_t position = 0;
    for (uint32_t i = 0; i < message_count; ++i) {
        const int written = snprintf(
            output + position,
            required + 1 - position,
            "<|%s|>\n%s\n",
            messages[i].role,
            messages[i].content);
        if (written < 0) {
            free(output);
            pthread_mutex_unlock(&model->inference_lock);
            return -1;
        }
        position += (size_t)written;
    }
    if (add_assistant_prompt) {
        const char assistant[] = "<|assistant|>\n";
        memcpy(output + position, assistant, sizeof(assistant) - 1);
        position += sizeof(assistant) - 1;
    }
    *formatted_text = output;
    *formatted_length = (uint32_t)position;
#endif
    pthread_mutex_unlock(&model->inference_lock);
    return 0;
}

void llm_free_text(char *text) {
    free(text);
}

int llm_warmup_shared_kv_with_cancel(
    llm_model_id_t model_id,
    const char *system_prompt,
    llm_cancel_callback cancel_callback,
    void *cancel_user_data) {
    if (!system_prompt || system_prompt[0] == '\0' ||
        !g_llm_manager.initialized) {
        return -1;
    }
    if (cancel_callback && cancel_callback(cancel_user_data)) {
        return -2;
    }

    pthread_mutex_lock(&g_llm_manager.lock);
    struct llm_model *model = find_model(model_id);
    if (!model) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }
    pthread_mutex_lock(&model->inference_lock);
    pthread_mutex_unlock(&g_llm_manager.lock);

    int index = find_shared_sys(model, system_prompt);
    if (index < 0) {
#ifdef USE_LLAMA_CPP
        llama_set_abort_callback(
            model->ctx, cancel_callback, cancel_user_data);
        const int cache_result =
            cache_system_prefix(model, system_prompt, &index);
        llama_set_abort_callback(model->ctx, NULL, NULL);
        if (cache_result != 0) {
            const bool cancelled =
                cancel_callback && cancel_callback(cancel_user_data);
            pthread_mutex_unlock(&model->inference_lock);
            return cancelled ? -2 : -1;
        }
#else
        index = select_shared_sys_slot(model);
        shared_sys_entry_t *entry = &model->shared_sys[index];
        if (entry->prompt_text) {
            free(entry->prompt_text);
            memset(entry, 0, sizeof(*entry));
            model->shared_sys_count--;
            model->stats.prefix_evictions++;
        }
        entry->seq_id = model->config.n_seq_max - 1 - index;
        entry->token_count = (int)strlen(system_prompt) / 4 + 1;
        entry->prompt_text = strdup(system_prompt);
        if (!entry->prompt_text) {
            pthread_mutex_unlock(&model->inference_lock);
            return -1;
        }
        touch_shared_sys(model, index);
        model->shared_sys_count++;
#endif
    }

    if (cancel_callback && cancel_callback(cancel_user_data)) {
        pthread_mutex_unlock(&model->inference_lock);
        return -2;
    }
    touch_shared_sys(model, index);
    int seq_id = model->shared_sys[index].seq_id;
    pthread_mutex_unlock(&model->inference_lock);
    return seq_id;
}

int llm_warmup_shared_kv(llm_model_id_t model_id,
                         const char *system_prompt) {
    return llm_warmup_shared_kv_with_cancel(
        model_id, system_prompt, NULL, NULL);
}

void llm_get_shared_kv_stats(int *total_shares, int *saved_tokens) {
    if (!total_shares || !saved_tokens) {
        return;
    }
    *total_shares = 0;
    *saved_tokens = 0;
    if (!g_llm_manager.initialized) {
        return;
    }

    pthread_mutex_lock(&g_llm_manager.lock);
    for (int i = 0; i < MAX_MODELS; ++i) {
        struct llm_model *model = &g_llm_manager.models[i];
        if (!model->used) {
            continue;
        }
        pthread_mutex_lock(&model->inference_lock);
        for (int j = 0; j < MAX_SHARED_SYS; ++j) {
            if (!model->shared_sys[j].prompt_text) {
                continue;
            }
            *total_shares += model->shared_sys[j].share_count;
            *saved_tokens += model->shared_sys[j].saved_tokens;
        }
        pthread_mutex_unlock(&model->inference_lock);
    }
    pthread_mutex_unlock(&g_llm_manager.lock);
}

void llm_clear_seq(llm_model_id_t model_id, int seq_id) {
    if (!g_llm_manager.initialized) {
        return;
    }

    pthread_mutex_lock(&g_llm_manager.lock);
    struct llm_model *model = find_model(model_id);
    if (!model || seq_id <= 0 || seq_id >= private_seq_limit(model)) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return;
    }
    int index = model_index(model);
    pthread_mutex_lock(&model->inference_lock);
    pthread_mutex_unlock(&g_llm_manager.lock);

#ifdef USE_LLAMA_CPP
    llama_memory_seq_rm(llama_get_memory(model->ctx), seq_id, -1, -1);
#endif
    g_llm_manager.seq_states[index][seq_id].current_pos = 0;
    g_llm_manager.seq_states[index][seq_id].in_use = false;
    pthread_mutex_unlock(&model->inference_lock);
}

int llm_checkpoint_seq(llm_model_id_t model_id,
                       int seq_id,
                       const char *path,
                       int *position,
                       uint64_t *bytes_written) {
    if (!path || !position || !bytes_written ||
        !g_llm_manager.initialized) {
        return -1;
    }
    *position = 0;
    *bytes_written = 0;

    pthread_mutex_lock(&g_llm_manager.lock);
    struct llm_model *model = find_model(model_id);
    if (!model || seq_id <= 0 || seq_id >= private_seq_limit(model)) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }
    const int index = model_index(model);
    pthread_mutex_lock(&model->inference_lock);
    pthread_mutex_unlock(&g_llm_manager.lock);

    seq_state_t *state = &g_llm_manager.seq_states[index][seq_id];
    if (!state->in_use || state->current_pos <= 0) {
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }

#ifdef USE_LLAMA_CPP
    const size_t written = llama_state_seq_save_file(
        model->ctx, path, seq_id, NULL, 0);
    if (written == 0) {
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    *bytes_written = (uint64_t)written;
#else
    FILE *checkpoint = fopen(path, "wb");
    if (!checkpoint) {
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    const size_t written =
        fwrite(&state->current_pos, sizeof(state->current_pos), 1, checkpoint);
    const int close_result = fclose(checkpoint);
    if (written != 1 || close_result != 0) {
        remove(path);
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    *bytes_written = sizeof(state->current_pos);
#endif
    *position = state->current_pos;
    pthread_mutex_unlock(&model->inference_lock);
    return 0;
}

int llm_restore_seq(llm_model_id_t model_id,
                    int seq_id,
                    const char *path,
                    int position,
                    uint64_t *bytes_read) {
    if (!path || position <= 0 || !bytes_read ||
        !g_llm_manager.initialized) {
        return -1;
    }
    *bytes_read = 0;

    pthread_mutex_lock(&g_llm_manager.lock);
    struct llm_model *model = find_model(model_id);
    if (!model || seq_id <= 0 || seq_id >= private_seq_limit(model)) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }
    const int index = model_index(model);
    pthread_mutex_lock(&model->inference_lock);
    pthread_mutex_unlock(&g_llm_manager.lock);

#ifdef USE_LLAMA_CPP
    llama_memory_t memory = llama_get_memory(model->ctx);
    llama_memory_seq_rm(memory, seq_id, -1, -1);
    size_t token_count = 0;
    const size_t read = llama_state_seq_load_file(
        model->ctx, path, seq_id, NULL, 0, &token_count);
    if (read == 0) {
        llama_memory_seq_rm(memory, seq_id, -1, -1);
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    *bytes_read = (uint64_t)read;
#else
    FILE *checkpoint = fopen(path, "rb");
    int stored_position = 0;
    if (!checkpoint) {
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    const size_t read =
        fread(&stored_position, sizeof(stored_position), 1, checkpoint);
    const int close_result = fclose(checkpoint);
    if (read != 1 || close_result != 0 ||
        stored_position != position) {
        pthread_mutex_unlock(&model->inference_lock);
        return -1;
    }
    *bytes_read = sizeof(stored_position);
#endif
    g_llm_manager.seq_states[index][seq_id].current_pos = position;
    g_llm_manager.seq_states[index][seq_id].in_use = true;
    pthread_mutex_unlock(&model->inference_lock);
    return 0;
}

int llm_get_model_info(llm_model_id_t model_id,
                       char *info,
                       uint32_t info_size) {
    if (!info || info_size == 0 || !g_llm_manager.initialized) {
        return -1;
    }

    pthread_mutex_lock(&g_llm_manager.lock);
    struct llm_model *model = find_model(model_id);
    if (!model) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }
    pthread_mutex_lock(&model->inference_lock);
    pthread_mutex_unlock(&g_llm_manager.lock);

    snprintf(info, info_size,
             "model_id=%" PRIu64 "\npath=%s\ncontext=%d\nbatch=%d\n"
             "threads=%d\nsequences=%d\nshared_prefixes=%d\n"
             "requests=%" PRIu64 "\ntokens=%" PRIu64,
             (uint64_t)model->id, model->config.model_path,
             model->config.n_ctx,
             model->config.n_batch, model->config.n_threads,
             private_seq_limit(model) - 1, model->shared_sys_count,
             (uint64_t)model->stats.total_requests,
             (uint64_t)model->stats.total_tokens);
    pthread_mutex_unlock(&model->inference_lock);
    return 0;
}

int llm_get_stats(llm_model_id_t model_id, struct llm_stats *stats) {
    if (!stats || !g_llm_manager.initialized) {
        return -1;
    }

    pthread_mutex_lock(&g_llm_manager.lock);
    struct llm_model *model = find_model(model_id);
    if (!model) {
        pthread_mutex_unlock(&g_llm_manager.lock);
        return -1;
    }
    pthread_mutex_lock(&model->inference_lock);
    pthread_mutex_unlock(&g_llm_manager.lock);
    *stats = model->stats;
    pthread_mutex_unlock(&model->inference_lock);
    return 0;
}
