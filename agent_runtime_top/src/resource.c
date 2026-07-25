/**
 * @file resource.c
 * @brief 资源抽象层 - 实现文件
 * 
 * 实现了统一的资源管理框架，提供资源的注册、发现、分配和释放功能。
 * 
 * 设计要点：
 *   - 资源通过描述符+操作集的模式注册到全局资源表
 *   - 资源分配采用首次适应策略，查找第一个满足条件的资源
 *   - 资源句柄作为已分配资源的引用，用于后续操作和释放
 *   - 所有操作通过互斥锁保护，支持多线程并发访问
 */

#include "resource.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/** 最大可注册的资源数量 */
#define MAX_RESOURCES  256

/**
 * @brief 全局资源管理表
 */
static struct {
    struct resource_desc descs[MAX_RESOURCES];  /* 资源描述符数组 */
    struct resource_ops *ops[MAX_RESOURCES];    /* 对应的操作集指针数组 */
    uint32_t count;                             /* 当前注册的资源数量 */
    pthread_mutex_t lock;                       /* 互斥锁 */
    bool initialized;                           /* 是否已初始化 */
} g_resource_table;

/* ========== 初始化与销毁 ========== */

int resource_init(void) {
    if (g_resource_table.initialized) {
        return -1;
    }
    
    /* 清零并初始化互斥锁 */
    memset(&g_resource_table, 0, sizeof(g_resource_table));
    pthread_mutex_init(&g_resource_table.lock, NULL);
    g_resource_table.initialized = true;
    
    return 0;
}

void resource_destroy(void) {
    if (!g_resource_table.initialized) {
        return;
    }
    
    pthread_mutex_lock(&g_resource_table.lock);
    
    /* 遍历所有已注册资源，调用销毁回调并释放私有数据 */
    for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
        if (g_resource_table.descs[i].id != 0) {
            /* 调用资源的销毁回调 */
            if (g_resource_table.ops[i] && g_resource_table.ops[i]->destroy) {
                g_resource_table.ops[i]->destroy(&g_resource_table.descs[i]);
            }
            /* 释放私有数据 */
            if (g_resource_table.descs[i].private_data) {
                free(g_resource_table.descs[i].private_data);
            }
        }
    }
    
    pthread_mutex_unlock(&g_resource_table.lock);
    pthread_mutex_destroy(&g_resource_table.lock);
    
    memset(&g_resource_table, 0, sizeof(g_resource_table));
}

/* ========== 资源注册 ========== */

int resource_register(struct resource_desc *desc, struct resource_ops *ops) {
    if (!desc || !ops || !g_resource_table.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_resource_table.lock);
    
    /* 查找空闲槽位 */
    for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
        if (g_resource_table.descs[i].id == 0) {
            /* 复制资源描述符到全局表 */
            memcpy(&g_resource_table.descs[i], desc, sizeof(struct resource_desc));
            g_resource_table.ops[i] = ops;
            
            /* 调用资源的初始化回调 */
            if (ops->init) {
                ops->init(&g_resource_table.descs[i]);
            }
            
            g_resource_table.count++;
            
            pthread_mutex_unlock(&g_resource_table.lock);
            return 0;
        }
    }
    
    /* 资源表已满 */
    pthread_mutex_unlock(&g_resource_table.lock);
    return -1;
}

int resource_unregister(resource_id_t id) {
    if (!g_resource_table.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_resource_table.lock);
    
    /* 查找目标资源 */
    for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
        if (g_resource_table.descs[i].id == id) {
            /* 调用销毁回调 */
            if (g_resource_table.ops[i] && g_resource_table.ops[i]->destroy) {
                g_resource_table.ops[i]->destroy(&g_resource_table.descs[i]);
            }
            
            /* 释放私有数据 */
            if (g_resource_table.descs[i].private_data) {
                free(g_resource_table.descs[i].private_data);
            }
            
            /* 清空槽位 */
            memset(&g_resource_table.descs[i], 0, sizeof(struct resource_desc));
            g_resource_table.ops[i] = NULL;
            g_resource_table.count--;
            
            pthread_mutex_unlock(&g_resource_table.lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&g_resource_table.lock);
    return -1;  /* 未找到 */
}

/* ========== 资源发现 ========== */

int resource_discover(enum resource_type type, 
                      struct resource_desc **descs,
                      uint32_t *count) {
    if (!g_resource_table.initialized || !count) {
        return -1;
    }
    
    uint32_t found = 0;
    struct resource_desc *result = NULL;
    
    pthread_mutex_lock(&g_resource_table.lock);
    
    /* 第一遍：统计匹配类型的资源数量 */
    for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
        if (g_resource_table.descs[i].id != 0 &&
            g_resource_table.descs[i].type == type) {
            found++;
        }
    }
    
    /* 第二遍：复制匹配的资源描述符到输出数组 */
    if (found > 0 && descs) {
        result = calloc(found, sizeof(struct resource_desc));
        if (result) {
            uint32_t idx = 0;
            for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
                if (g_resource_table.descs[i].id != 0 &&
                    g_resource_table.descs[i].type == type) {
                    memcpy(&result[idx], &g_resource_table.descs[i],
                           sizeof(struct resource_desc));
                    idx++;
                }
            }
        }
    }
    
    pthread_mutex_unlock(&g_resource_table.lock);
    
    *count = found;
    if (descs) {
        *descs = result;  /* 调用者需free返回的数组 */
    }
    
    return 0;
}

/* ========== 资源分配与释放 ========== */

int resource_allocate(const struct resource_request *request,
                      struct resource_handle *handle) {
    if (!request || !handle || !g_resource_table.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_resource_table.lock);
    
    /* 遍历所有资源，查找类型匹配且空闲的资源 */
    for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
        if (g_resource_table.descs[i].id != 0 &&
            g_resource_table.descs[i].type == request->type &&
            g_resource_table.descs[i].state == RESOURCE_STATE_IDLE) {
            
            /* 检查可用容量是否足够 */
            uint64_t available = g_resource_table.descs[i].capacity - 
                                g_resource_table.descs[i].used;
            if (available >= request->size) {
                /* 调用资源的分配回调 */
                if (g_resource_table.ops[i] && g_resource_table.ops[i]->allocate) {
                    int ret = g_resource_table.ops[i]->allocate(
                        &g_resource_table.descs[i], request->size);
                    if (ret != 0) {
                        continue;  /* 分配失败，尝试下一个 */
                    }
                }
                
                /* 更新资源使用量和状态 */
                g_resource_table.descs[i].used += request->size;
                g_resource_table.descs[i].state = RESOURCE_STATE_BUSY;
                
                /* 填充输出句柄 */
                handle->id = g_resource_table.descs[i].id;
                handle->desc = &g_resource_table.descs[i];
                handle->context = NULL;
                handle->allocated_size = request->size;
                
                pthread_mutex_unlock(&g_resource_table.lock);
                return 0;
            }
        }
    }
    
    /* 没有找到满足条件的资源 */
    pthread_mutex_unlock(&g_resource_table.lock);
    return -1;
}

int resource_release(struct resource_handle *handle) {
    if (!handle || !g_resource_table.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_resource_table.lock);
    
    /* 根据句柄中的ID查找资源 */
    for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
        if (g_resource_table.descs[i].id == handle->id) {
            /* 调用资源的释放回调 */
            if (g_resource_table.ops[i] && g_resource_table.ops[i]->release) {
                g_resource_table.ops[i]->release(
                    &g_resource_table.descs[i], handle->allocated_size);
            }
            
            /* 减少已使用量 */
            g_resource_table.descs[i].used -= handle->allocated_size;
            /* 如果完全释放，恢复为空闲状态 */
            if (g_resource_table.descs[i].used == 0) {
                g_resource_table.descs[i].state = RESOURCE_STATE_IDLE;
            }
            
            /* 清空句柄 */
            memset(handle, 0, sizeof(struct resource_handle));
            
            pthread_mutex_unlock(&g_resource_table.lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&g_resource_table.lock);
    return -1;  /* 未找到资源 */
}

/* ========== 查询 ========== */

int resource_get_info(resource_id_t id, struct resource_desc *desc) {
    if (!desc || !g_resource_table.initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&g_resource_table.lock);
    
    for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
        if (g_resource_table.descs[i].id == id) {
            memcpy(desc, &g_resource_table.descs[i], sizeof(struct resource_desc));
            pthread_mutex_unlock(&g_resource_table.lock);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&g_resource_table.lock);
    return -1;
}