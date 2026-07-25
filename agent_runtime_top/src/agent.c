/**
 * @file agent.c
 * @brief Agent执行抽象模型 - 实现文件
 * 
 * 实现了Agent的完整生命周期管理，包括创建、初始化、启动、停止和销毁。
 * 使用全局Agent表管理所有Agent实例，通过互斥锁保证多线程安全。
 */

#include "agent.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/** 最大同时管理的Agent数量 */
#define MAX_AGENTS  256

/**
 * @brief 全局Agent管理表
 * 
 * 使用静态数组存储所有Agent，支持O(n)查找。
 * 在实际系统中可替换为哈希表以提升查找性能。
 */
static struct {
    struct agent agents[MAX_AGENTS];    /* Agent数组 */
    uint32_t count;                     /* 当前活跃Agent数量 */
    agent_id_t next_id;                 /* 下一个可分配的Agent ID */
    pthread_mutex_t lock;               /* 互斥锁，保护并发访问 */
    bool initialized;                   /* 是否已初始化 */
} g_agent_table;

/**
 * @brief 获取当前时间（毫秒）
 * @return 单调递增的时间戳
 */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ========== Runtime全局管理 ========== */

int agent_runtime_init(void) {
    /* 防止重复初始化 */
    if (g_agent_table.initialized) {
        return -1;
    }
    
    /* 清零并初始化互斥锁 */
    memset(&g_agent_table, 0, sizeof(g_agent_table));
    pthread_mutex_init(&g_agent_table.lock, NULL);
    g_agent_table.next_id = 1;  /* Agent ID从1开始（0为无效ID） */
    g_agent_table.initialized = true;
    
    return 0;
}

void agent_runtime_destroy(void) {
    if (!g_agent_table.initialized) {
        return;
    }
    
    pthread_mutex_lock(&g_agent_table.lock);
    
    /* 遍历释放所有Agent的私有上下文 */
    for (uint32_t i = 0; i < MAX_AGENTS; i++) {
        if (g_agent_table.agents[i].id != AGENT_ID_INVALID) {
            if (g_agent_table.agents[i].context) {
                free(g_agent_table.agents[i].context);
            }
        }
    }
    
    pthread_mutex_unlock(&g_agent_table.lock);
    pthread_mutex_destroy(&g_agent_table.lock);
    
    memset(&g_agent_table, 0, sizeof(g_agent_table));
}

/* ========== Agent生命周期管理 ========== */

agent_id_t agent_create(const struct agent_config *config) {
    /* 参数校验 */
    if (!config || !g_agent_table.initialized) {
        return AGENT_ID_INVALID;
    }
    
    pthread_mutex_lock(&g_agent_table.lock);
    
    /* 在Agent表中寻找空闲槽位 */
    for (uint32_t i = 0; i < MAX_AGENTS; i++) {
        if (g_agent_table.agents[i].id == AGENT_ID_INVALID) {
            struct agent *a = &g_agent_table.agents[i];
            
            /* 初始化Agent属性 */
            a->id = g_agent_table.next_id++;
            strncpy(a->name, config->name, AGENT_NAME_MAX - 1);
            a->name[AGENT_NAME_MAX - 1] = '\0';  /* 确保字符串以'\0'结尾 */
            a->state = AGENT_STATE_CREATED;        /* 初始状态为CREATED */
            a->priority = config->priority;
            a->create_time = get_time_ms();
            a->last_active = a->create_time;
            a->context = NULL;  /* 上下文由ops->init初始化 */
            a->ops = NULL;      /* 操作集由外部设置 */
            
            g_agent_table.count++;
            
            pthread_mutex_unlock(&g_agent_table.lock);
            return a->id;
        }
    }
    
    /* Agent表已满 */
    pthread_mutex_unlock(&g_agent_table.lock);
    return AGENT_ID_INVALID;
}

int agent_destroy(agent_id_t id) {
    if (!g_agent_table.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_agent_table.lock);
    
    for (uint32_t i = 0; i < MAX_AGENTS; i++) {
        if (g_agent_table.agents[i].id == id) {
            struct agent *a = &g_agent_table.agents[i];
            
            /* 调用清理回调释放Agent资源 */
            if (a->ops && a->ops->cleanup) {
                a->ops->cleanup(a->context);
            }
            
            /* 释放私有上下文内存 */
            if (a->context) {
                free(a->context);
                a->context = NULL;
            }
            
            /* 标记槽位为空闲 */
            a->id = AGENT_ID_INVALID;
            a->state = AGENT_STATE_CREATED;
            g_agent_table.count--;
            
            pthread_mutex_unlock(&g_agent_table.lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&g_agent_table.lock);
    return -1;
}

int agent_init(agent_id_t id) {
    if (!g_agent_table.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_agent_table.lock);
    
    for (uint32_t i = 0; i < MAX_AGENTS; i++) {
        if (g_agent_table.agents[i].id == id) {
            struct agent *a = &g_agent_table.agents[i];
            
            /* 只有CREATED状态的Agent才能初始化 */
            if (a->state != AGENT_STATE_CREATED) {
                pthread_mutex_unlock(&g_agent_table.lock);
                return -1;
            }
            
            /* 调用初始化回调 */
            if (a->ops && a->ops->init) {
                int ret = a->ops->init(a->context);
                if (ret != 0) {
                    /* 初始化失败，标记为ERROR状态 */
                    a->state = AGENT_STATE_ERROR;
                    pthread_mutex_unlock(&g_agent_table.lock);
                    return ret;
                }
            }
            
            /* 初始化成功，转换到READY状态 */
            a->state = AGENT_STATE_READY;
            a->last_active = get_time_ms();
            
            pthread_mutex_unlock(&g_agent_table.lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&g_agent_table.lock);
    return -1;
}

int agent_start(agent_id_t id) {
    if (!g_agent_table.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_agent_table.lock);
    
    for (uint32_t i = 0; i < MAX_AGENTS; i++) {
        if (g_agent_table.agents[i].id == id) {
            struct agent *a = &g_agent_table.agents[i];
            
            /* 只有READY状态的Agent才能启动 */
            if (a->state != AGENT_STATE_READY) {
                pthread_mutex_unlock(&g_agent_table.lock);
                return -1;
            }
            
            /* READY → RUNNING */
            a->state = AGENT_STATE_RUNNING;
            a->last_active = get_time_ms();
            
            pthread_mutex_unlock(&g_agent_table.lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&g_agent_table.lock);
    return -1;
}

int agent_stop(agent_id_t id) {
    if (!g_agent_table.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_agent_table.lock);
    
    for (uint32_t i = 0; i < MAX_AGENTS; i++) {
        if (g_agent_table.agents[i].id == id) {
            struct agent *a = &g_agent_table.agents[i];
            
            /* 只有RUNNING或WAITING状态的Agent才能停止 */
            if (a->state != AGENT_STATE_RUNNING && 
                a->state != AGENT_STATE_WAITING) {
                pthread_mutex_unlock(&g_agent_table.lock);
                return -1;
            }
            
            /* RUNNING/WAITING → COMPLETED */
            a->state = AGENT_STATE_COMPLETED;
            a->last_active = get_time_ms();
            
            pthread_mutex_unlock(&g_agent_table.lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&g_agent_table.lock);
    return -1;
}

int agent_wait(agent_id_t id, int timeout_ms) {
    if (!g_agent_table.initialized) {
        return -1;
    }
    
    uint64_t start = get_time_ms();
    
    /* 轮询等待Agent完成 */
    while (1) {
        pthread_mutex_lock(&g_agent_table.lock);
        
        for (uint32_t i = 0; i < MAX_AGENTS; i++) {
            if (g_agent_table.agents[i].id == id) {
                struct agent *a = &g_agent_table.agents[i];
                
                /* Agent已完成或出错，立即返回 */
                if (a->state == AGENT_STATE_COMPLETED ||
                    a->state == AGENT_STATE_ERROR) {
                    pthread_mutex_unlock(&g_agent_table.lock);
                    return 0;
                }
                
                pthread_mutex_unlock(&g_agent_table.lock);
                
                /* 检查是否超时 */
                if (timeout_ms > 0) {
                    uint64_t elapsed = get_time_ms() - start;
                    if (elapsed >= (uint64_t)timeout_ms) {
                        return -1;  /* 超时 */
                    }
                }
                
                /* 短暂休眠避免忙等 */
                usleep(1000);  /* 1ms */
                goto next;
            }
        }
        
        /* 未找到Agent */
        pthread_mutex_unlock(&g_agent_table.lock);
        return -1;
        
next:
        continue;
    }
}

/* ========== 查询与设置 ========== */

enum agent_state agent_get_state(agent_id_t id) {
    if (!g_agent_table.initialized) {
        return AGENT_STATE_CREATED;
    }
    
    pthread_mutex_lock(&g_agent_table.lock);
    
    for (uint32_t i = 0; i < MAX_AGENTS; i++) {
        if (g_agent_table.agents[i].id == id) {
            enum agent_state state = g_agent_table.agents[i].state;
            pthread_mutex_unlock(&g_agent_table.lock);
            return state;
        }
    }
    
    pthread_mutex_unlock(&g_agent_table.lock);
    return AGENT_STATE_CREATED;  /* 未找到时返回默认值 */
}

const char *agent_state_str(enum agent_state state) {
    /* 状态名称映射表 */
    static const char *state_names[] = {
        "CREATED",
        "READY",
        "RUNNING",
        "WAITING",
        "COMPLETED",
        "ERROR"
    };
    
    if (state < AGENT_STATE_MAX) {
        return state_names[state];
    }
    return "UNKNOWN";
}

int agent_set_priority(agent_id_t id, uint32_t priority) {
    if (!g_agent_table.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_agent_table.lock);
    
    for (uint32_t i = 0; i < MAX_AGENTS; i++) {
        if (g_agent_table.agents[i].id == id) {
            g_agent_table.agents[i].priority = priority;
            pthread_mutex_unlock(&g_agent_table.lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&g_agent_table.lock);
    return -1;
}