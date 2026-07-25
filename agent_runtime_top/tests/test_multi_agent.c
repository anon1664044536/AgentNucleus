/**
 * @file test_multi_agent.c
 * @brief 多Agent共享KV Cache + 多轮对话测试
 * 
 * 场景：Yummy House餐厅团队
 * - 3个Agent共享同一系统提示词（餐厅背景信息）
 * - 每个Agent进行3轮对话
 * - 验证KV Cache共享和多轮对话功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "llm.h"
#include "agent.h"

/* ============================================================
 * 测试配置
 * ============================================================ */

/* 共享系统提示词 */
static const char *SYSTEM_PROMPT = 
    "You are a team running a small restaurant called Yummy House. "
    "The restaurant serves Chinese and Western food, open 11am-9pm, "
    "has 20 seats, and accepts cash and mobile payment. "
    "Answer customer questions directly and briefly based on this information.";

/* 3个Agent的定义 */
typedef struct {
    const char *name;
    int seq_id;
    const char *questions[3];
} agent_def_t;

static agent_def_t agents[3] = {
    {
        .name = "Agent1-前台接待",
        .seq_id = 1,
        .questions = {
            "What are our opening hours?",
            "How many seats do we have?",
            "What payment methods do we accept?"
        }
    },
    {
        .name = "Agent2-厨师",
        .seq_id = 2,
        .questions = {
            "What type of food do we serve?",
            "How many customers can we serve at once?",
            "When is our busiest time?"
        }
    },
    {
        .name = "Agent3-采购员",
        .seq_id = 3,
        .questions = {
            "What are our business hours?",
            "What payment methods should I set up for suppliers?",
            "How much ingredients should I prepare for 20 seats?"
        }
    }
};

/* ============================================================
 * 辅助函数
 * ============================================================ */

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void print_separator(void) {
    printf("────────────────────────────────────────────────────────\n");
}

/* ============================================================
 * 测试函数
 * ============================================================ */

/**
 * @brief 测试1：预热共享KV Cache
 */
static llm_model_id_t test_warmup(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║       测试1：预热共享KV Cache（系统提示词编码）          ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    
    /* 初始化LLM */
    llm_manager_init();
    
    /* 加载模型 */
    struct llm_model_config model_config = {
        .model_path = "qwen2-0_5b.gguf",
        .n_ctx = 4096,      /* 增大上下文以支持多Agent多轮 */
        .n_threads = 4,
        .n_gpu_layers = 0,
        .temperature = 0.7f,
        .top_p = 0.9f,
        .max_tokens = 64,  /* 每轮生成64 tokens */
        .use_mmap = true,
        .use_mlock = false
    };
    
    llm_model_id_t model_id;
    if (llm_load_model(LLM_BACKEND_LLAMA_CPP, &model_config, &model_id) != 0) {
        printf("模型加载失败!\n");
        return 0;
    }
    printf("模型加载成功\n\n");
    
    /* 预热共享KV Cache */
    printf("预热系统提示词的KV Cache...\n");
    printf("系统提示词: \"%s\"\n\n", SYSTEM_PROMPT);
    
    uint64_t start = get_time_ms();
    int shared_seq = llm_warmup_shared_kv(model_id, SYSTEM_PROMPT);
    uint64_t elapsed = get_time_ms() - start;
    
    printf("预热完成: seq_id=%d, 耗时 %lu ms\n\n", shared_seq, (unsigned long)elapsed);
    
    return model_id;
}

/**
 * @brief 测试2：多Agent多轮对话
 */
static void test_multi_agent(llm_model_id_t model_id) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║         测试2：多Agent多轮对话（KV Cache共享）           ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    
    printf("场景: Yummy House餐厅团队协作\n");
    printf("系统提示词: 共享（包含餐厅背景信息）\n");
    printf("3个Agent: 前台接待(seq=1), 厨师(seq=2), 采购员(seq=3)\n");
    printf("每个Agent: 3轮对话\n");
    print_separator();
    
    uint64_t total_start = get_time_ms();
    
    /* 逐Agent进行对话 */
    for (int a = 0; a < 3; a++) {
        agent_def_t *agent = &agents[a];
        
        printf("\n");
        printf("┌────────────────────────────────────────────────────────┐\n");
        printf("│ %s (seq_id=%d)\n", agent->name, agent->seq_id);
        printf("└────────────────────────────────────────────────────────┘\n");
        
        for (int round = 0; round < 3; round++) {
            printf("\n  [第%d轮] 问题: \"%s\"\n", round + 1, agent->questions[round]);
            
            /* 构造请求 */
            struct llm_request request = {
                .session = 0,
                .prompt = agent->questions[round],
                .system_prompt = (round == 0) ? SYSTEM_PROMPT : NULL,
                .seq_id = agent->seq_id,
                .is_first_turn = (round == 0),
                .temperature = 0.7f,
                .top_p = 0.9f,
                .max_tokens = 64,
                .stream = false
            };
            
            /* 执行推理 */
            uint64_t start = get_time_ms();
            struct llm_response response;
            
            if (llm_inference(model_id, &request, &response) != 0) {
                printf("  推理失败!\n");
                continue;
            }
            
            uint64_t elapsed = get_time_ms() - start;
            
            /* 输出结果 */
            printf("  回答: %.200s\n", response.text);
            printf("  统计: 生成 %d tokens, 耗时 %lu ms\n",
                   response.tokens_generated, (unsigned long)elapsed);
            
            llm_free_response(&response);
        }
        
        print_separator();
    }
    
    uint64_t total_elapsed = get_time_ms() - total_start;
    printf("\n总耗时: %lu ms\n", (unsigned long)total_elapsed);
}

/**
 * @brief 测试3：查看共享KV统计
 */
static void test_shared_stats(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║               测试3：共享KV Cache统计                    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    
    int total_shares = 0, saved_tokens = 0;
    llm_get_shared_kv_stats(&total_shares, &saved_tokens);
    
    printf("共享次数: %d\n", total_shares);
    printf("节省token: %d\n", saved_tokens);
    printf("\n说明: Agent2和Agent3复用了Agent1的系统提示词KV Cache\n");
    printf("      节省了2次系统提示词的编码计算\n");
}

/* ============================================================
 * 主函数
 * ============================================================ */

int main(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║     多Agent共享KV Cache + 多轮对话 集成测试              ║\n");
    printf("║     场景: Yummy House餐厅团队协作                       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    
    /* 测试1: 预热共享KV */
    llm_model_id_t model_id = test_warmup();
    if (model_id == 0) {
        printf("预热失败，退出\n");
        return 1;
    }
    
    /* 测试2: 多Agent多轮对话 */
    test_multi_agent(model_id);
    
    /* 测试3: 共享统计 */
    test_shared_stats();
    
    /* 清理 */
    printf("\n清理资源...\n");
    llm_unload_model(model_id);
    llm_manager_destroy();
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║                    测试完成!                             ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    
    return 0;
}
