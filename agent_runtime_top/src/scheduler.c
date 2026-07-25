/**
 * @file scheduler.c
 * @brief Agent调度器 - 实现文件
 * 
 * 实现了基于循环队列的Agent调度器，支持：
 *   - Agent入队/出队
 *   - 下一个Agent选取
 *   - 阻塞/唤醒机制
 *   - 调度统计
 * 
 * 队列使用循环数组实现，通过互斥锁+条件变量实现线程安全的阻塞调度。
 */

#include "scheduler.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/**
 * @brief 调度器全局状态
 */
static struct {
    agent_id_t *queue;                  /* 就绪队列（循环数组） */
    uint32_t queue_size;                /* 队列容量 */
    uint32_t head;                      /* 队列头部索引 */
    uint32_t tail;                      /* 队列尾部索引 */
    uint32_t count;                     /* 当前队列中的Agent数量 */
    struct scheduler_config config;     /* 调度配置 */
    struct scheduler_stats stats;       /* 统计信息 */
    pthread_mutex_t lock;               /* 互斥锁 */
    pthread_cond_t cond;                /* 条件变量（用于阻塞等待） */
    bool running;                       /* 调度器是否运行中 */
    bool initialized;                   /* 是否已初始化 */
} g_scheduler;

/* ========== 初始化与销毁 ========== */

int scheduler_init(const struct scheduler_config *config) {
    if (g_scheduler.initialized) {
        return -1;
    }
    
    if (!config || config->max_agents == 0) {
        return -1;
    }
    
    memset(&g_scheduler, 0, sizeof(g_scheduler));
    
    /* 分配就绪队列 */
    g_scheduler.queue_size = config->max_agents;
    g_scheduler.queue = calloc(g_scheduler.queue_size, sizeof(agent_id_t));
    if (!g_scheduler.queue) {
        return -1;  /* 内存分配失败 */
    }
    
    /* 保存配置 */
    memcpy(&g_scheduler.config, config, sizeof(struct scheduler_config));
    
    /* 初始化同步原语 */
    pthread_mutex_init(&g_scheduler.lock, NULL);
    pthread_cond_init(&g_scheduler.cond, NULL);
    
    /* 初始化队列状态 */
    g_scheduler.head = 0;
    g_scheduler.tail = 0;
    g_scheduler.count = 0;
    g_scheduler.running = false;
    g_scheduler.initialized = true;
    
    return 0;
}

void scheduler_destroy(void) {
    if (!g_scheduler.initialized) {
        return;
    }
    
    scheduler_stop();  /* 先停止调度器 */
    
    free(g_scheduler.queue);  /* 释放队列内存 */
    
    pthread_mutex_destroy(&g_scheduler.lock);
    pthread_cond_destroy(&g_scheduler.cond);
    
    memset(&g_scheduler, 0, sizeof(g_scheduler));
}

/* ========== Agent队列管理 ========== */

int scheduler_add_agent(agent_id_t id) {
    if (!g_scheduler.initialized || id == 0) {
        return -1;
    }
    
    pthread_mutex_lock(&g_scheduler.lock);
    
    /* 检查队列是否已满 */
    if (g_scheduler.count >= g_scheduler.queue_size) {
        pthread_mutex_unlock(&g_scheduler.lock);
        return -1;
    }
    
    /* 将Agent插入队尾 */
    g_scheduler.queue[g_scheduler.tail] = id;
    /* 循环递增tail指针 */
    g_scheduler.tail = (g_scheduler.tail + 1) % g_scheduler.queue_size;
    g_scheduler.count++;
    
    /* 通知可能在pick_next中等待的线程 */
    pthread_cond_signal(&g_scheduler.cond);
    pthread_mutex_unlock(&g_scheduler.lock);
    
    return 0;
}

int scheduler_remove_agent(agent_id_t id) {
    if (!g_scheduler.initialized || id == 0) {
        return -1;
    }
    
    pthread_mutex_lock(&g_scheduler.lock);
    
    /* 遍历队列查找目标Agent */
    for (uint32_t i = 0; i < g_scheduler.count; i++) {
        uint32_t idx = (g_scheduler.head + i) % g_scheduler.queue_size;
        if (g_scheduler.queue[idx] == id) {
            /* 找到后，将后面的元素依次前移 */
            for (uint32_t j = i; j < g_scheduler.count - 1; j++) {
                uint32_t curr = (g_scheduler.head + j) % g_scheduler.queue_size;
                uint32_t next = (g_scheduler.head + j + 1) % g_scheduler.queue_size;
                g_scheduler.queue[curr] = g_scheduler.queue[next];
            }
            
            /* 尾指针前移，计数减一 */
            g_scheduler.tail = (g_scheduler.tail - 1 + g_scheduler.queue_size) % 
                              g_scheduler.queue_size;
            g_scheduler.count--;
            
            pthread_mutex_unlock(&g_scheduler.lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&g_scheduler.lock);
    return -1;  /* 未找到 */
}

int scheduler_update_agent(agent_id_t id) {
    if (!g_scheduler.initialized || id == 0) {
        return -1;
    }
    
    pthread_mutex_lock(&g_scheduler.lock);
    
    /* 查找Agent并将其移到队首（提升调度优先级） */
    for (uint32_t i = 0; i < g_scheduler.count; i++) {
        uint32_t idx = (g_scheduler.head + i) % g_scheduler.queue_size;
        if (g_scheduler.queue[idx] == id) {
            /* 将该Agent前面的元素依次后移 */
            for (uint32_t j = i; j > 0; j--) {
                uint32_t curr = (g_scheduler.head + j) % g_scheduler.queue_size;
                uint32_t prev = (g_scheduler.head + j - 1) % g_scheduler.queue_size;
                agent_id_t temp = g_scheduler.queue[curr];
                g_scheduler.queue[curr] = g_scheduler.queue[prev];
                g_scheduler.queue[prev] = temp;
            }
            
            pthread_mutex_unlock(&g_scheduler.lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&g_scheduler.lock);
    return -1;
}

/* ========== 调度操作 ========== */

agent_id_t scheduler_pick_next(void) {
    if (!g_scheduler.initialized) {
        return 0;
    }
    
    pthread_mutex_lock(&g_scheduler.lock);
    
    /* 如果队列为空且调度器在运行，则阻塞等待 */
    while (g_scheduler.count == 0 && g_scheduler.running) {
        pthread_cond_wait(&g_scheduler.cond, &g_scheduler.lock);
    }
    
    /* 调度器已停止且队列为空 */
    if (g_scheduler.count == 0) {
        pthread_mutex_unlock(&g_scheduler.lock);
        return 0;
    }
    
    /* 从队首取出下一个Agent */
    agent_id_t next = g_scheduler.queue[g_scheduler.head];
    g_scheduler.head = (g_scheduler.head + 1) % g_scheduler.queue_size;
    g_scheduler.count--;
    
    /* 更新统计信息 */
    g_scheduler.stats.total_schedules++;
    g_scheduler.stats.context_switches++;
    
    pthread_mutex_unlock(&g_scheduler.lock);
    
    return next;
}

int scheduler_yield(agent_id_t id) {
    if (!g_scheduler.initialized || id == 0) {
        return -1;
    }
    
    /* 让出CPU = 将自身重新加入就绪队列 */
    return scheduler_add_agent(id);
}

int scheduler_block(agent_id_t id) {
    if (!g_scheduler.initialized || id == 0) {
        return -1;
    }
    
    /* 阻塞 = 从就绪队列中移除 */
    return scheduler_remove_agent(id);
}

int scheduler_unblock(agent_id_t id) {
    if (!g_scheduler.initialized || id == 0) {
        return -1;
    }
    
    /* 唤醒 = 重新加入就绪队列 */
    return scheduler_add_agent(id);
}

/* ========== 调度器控制 ========== */

int scheduler_start(void) {
    if (!g_scheduler.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_scheduler.lock);
    g_scheduler.running = true;
    /* 广播唤醒所有在pick_next中等待的线程 */
    pthread_cond_broadcast(&g_scheduler.cond);
    pthread_mutex_unlock(&g_scheduler.lock);
    
    return 0;
}

int scheduler_stop(void) {
    if (!g_scheduler.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_scheduler.lock);
    g_scheduler.running = false;
    /* 广播唤醒等待线程，让它们检测到running=false后退出 */
    pthread_cond_broadcast(&g_scheduler.cond);
    pthread_mutex_unlock(&g_scheduler.lock);
    
    return 0;
}

/* ========== 统计 ========== */

int scheduler_get_stats(struct scheduler_stats *stats) {
    if (!g_scheduler.initialized || !stats) {
        return -1;
    }
    
    pthread_mutex_lock(&g_scheduler.lock);
    memcpy(stats, &g_scheduler.stats, sizeof(struct scheduler_stats));
    pthread_mutex_unlock(&g_scheduler.lock);
    
    return 0;
}