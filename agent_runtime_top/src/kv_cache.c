/**
 * @file kv_cache.c
 * @brief KV Cache管理模块 - 实现文件
 * 
 * 实现了多Agent系统中KV Cache的核心管理功能：
 *   1. 显存池化：将大块内存分割为固定大小的块进行管理
 *   2. 分配与释放：基于连续块分配的内存管理
 *   3. 共享机制：通过引用计数实现多Agent安全共享
 *   4. 压缩机制：按比例采样压缩KV Cache
 *   5. 复用机制：基于前缀匹配的上下文复用
 *   6. 统计信息：命中率、复用率等运行时指标
 */

#include "kv_cache.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/**
 * @brief KV Cache全局管理器
 * 
 * 管理所有KV Cache块，维护块状态和统计信息。
 */
static struct {
    struct kv_block *blocks;    /* 块描述符数组 */
    uint32_t block_count;       /* 块总数 */
    uint32_t block_size;        /* 每个块的大小（字节） */
    uint64_t total_size;        /* 总显存大小（字节） */
    uint64_t used_size;         /* 已使用大小（字节） */
    struct kv_stats stats;      /* 运行统计 */
    pthread_mutex_t lock;       /* 互斥锁 */
    bool initialized;           /* 是否已初始化 */
} g_kv_cache;

/**
 * @brief 获取当前时间（毫秒）
 */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * @brief 查找连续的空闲块
 * @param count 需要的连续块数量
 * @return 找到的起始块ID，未找到返回0
 * 
 * 使用首次适应（First Fit）算法，在块数组中查找连续count个空闲块。
 */
static kv_block_id_t find_free_block(uint32_t count) {
    /* 遍历所有块 */
    for (uint32_t i = 0; i < g_kv_cache.block_count; i++) {
        if (g_kv_cache.blocks[i].state == KV_BLOCK_FREE) {
            /* 找到一个空闲块，检查后续块是否也空闲 */
            bool found = true;
            for (uint32_t j = 1; j < count; j++) {
                if (i + j >= g_kv_cache.block_count ||
                    g_kv_cache.blocks[i + j].state != KV_BLOCK_FREE) {
                    found = false;
                    break;
                }
            }
            if (found) {
                return g_kv_cache.blocks[i].id;
            }
        }
    }
    return 0;  /* 未找到足够连续空闲块 */
}

/* ========== 初始化与销毁 ========== */

int kv_cache_init(const struct kv_config *config) {
    if (g_kv_cache.initialized) {
        return -1;
    }
    
    if (!config) {
        return -1;
    }
    
    /* 从配置中读取参数，使用默认值填充未指定的字段 */
    g_kv_cache.block_size = config->block_size;
    if (g_kv_cache.block_size == 0) {
        g_kv_cache.block_size = KV_BLOCK_SIZE_DEFAULT;
    }
    
    g_kv_cache.block_count = config->max_blocks;
    if (g_kv_cache.block_count == 0) {
        g_kv_cache.block_count = KV_MAX_BLOCKS;
    }
    
    g_kv_cache.total_size = config->total_size;
    if (g_kv_cache.total_size == 0) {
        g_kv_cache.total_size = (uint64_t)g_kv_cache.block_count * g_kv_cache.block_size;
    }
    
    /* 分配块描述符数组 */
    g_kv_cache.blocks = calloc(g_kv_cache.block_count, sizeof(struct kv_block));
    if (!g_kv_cache.blocks) {
        return -1;
    }
    
    /* 初始化每个块 */
    for (uint32_t i = 0; i < g_kv_cache.block_count; i++) {
        g_kv_cache.blocks[i].id = i + 1;  /* 块ID从1开始 */
        g_kv_cache.blocks[i].size = g_kv_cache.block_size;
        g_kv_cache.blocks[i].state = KV_BLOCK_FREE;
        g_kv_cache.blocks[i].ref_count = 0;
        g_kv_cache.blocks[i].owner = AGENT_ID_INVALID;
        g_kv_cache.blocks[i].last_access = 0;
        g_kv_cache.blocks[i].priority = 0;
        
        /* 分配块的数据缓冲区 */
        g_kv_cache.blocks[i].data = malloc(g_kv_cache.block_size);
        if (!g_kv_cache.blocks[i].data) {
            /* 分配失败，回滚已分配的内存 */
            for (uint32_t j = 0; j < i; j++) {
                free(g_kv_cache.blocks[j].data);
            }
            free(g_kv_cache.blocks);
            return -1;
        }
    }
    
    /* 初始化统计信息和同步原语 */
    memset(&g_kv_cache.stats, 0, sizeof(struct kv_stats));
    pthread_mutex_init(&g_kv_cache.lock, NULL);
    g_kv_cache.used_size = 0;
    g_kv_cache.initialized = true;
    
    return 0;
}

void kv_cache_destroy(void) {
    if (!g_kv_cache.initialized) {
        return;
    }
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    /* 释放所有块的数据缓冲区 */
    for (uint32_t i = 0; i < g_kv_cache.block_count; i++) {
        if (g_kv_cache.blocks[i].data) {
            free(g_kv_cache.blocks[i].data);
        }
    }
    
    /* 释放块描述符数组 */
    free(g_kv_cache.blocks);
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    pthread_mutex_destroy(&g_kv_cache.lock);
    
    memset(&g_kv_cache, 0, sizeof(g_kv_cache));
}

/* ========== 基本操作 ========== */

int kv_allocate(agent_id_t agent, uint32_t size, struct kv_range *range) {
    if (!g_kv_cache.initialized || !range) {
        return -1;
    }
    
    /* 计算需要的块数量（向上取整） */
    uint32_t blocks_needed = (size + g_kv_cache.block_size - 1) / g_kv_cache.block_size;
    if (blocks_needed == 0) {
        blocks_needed = 1;
    }
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    /* 查找连续的空闲块 */
    kv_block_id_t start = find_free_block(blocks_needed);
    if (start == 0) {
        pthread_mutex_unlock(&g_kv_cache.lock);
        return -1;  /* 空间不足 */
    }
    
    /* 填充输出范围 */
    range->start = start;
    range->count = blocks_needed;
    range->size = blocks_needed * g_kv_cache.block_size;
    
    /* 初始化分配的块 */
    for (uint32_t i = 0; i < blocks_needed; i++) {
        uint32_t idx = start - 1 + i;  /* 转换为数组索引 */
        g_kv_cache.blocks[idx].state = KV_BLOCK_ALLOCATED;
        g_kv_cache.blocks[idx].ref_count = 1;
        g_kv_cache.blocks[idx].owner = agent;
        g_kv_cache.blocks[idx].last_access = get_time_ms();
        memset(g_kv_cache.blocks[idx].data, 0, g_kv_cache.block_size);  /* 清零 */
    }
    
    /* 更新使用量和统计 */
    g_kv_cache.used_size += range->size;
    g_kv_cache.stats.total_allocated += range->size;
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    return 0;
}

int kv_free(struct kv_range *range) {
    if (!g_kv_cache.initialized || !range) {
        return -1;
    }
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    /* 逐块释放 */
    for (uint32_t i = 0; i < range->count; i++) {
        uint32_t idx = range->start - 1 + i;
        if (idx >= g_kv_cache.block_count) {
            pthread_mutex_unlock(&g_kv_cache.lock);
            return -1;
        }
        
        if (g_kv_cache.blocks[idx].state != KV_BLOCK_FREE) {
            /* 递减引用计数 */
            g_kv_cache.blocks[idx].ref_count--;
            /* 引用计数归零时真正释放 */
            if (g_kv_cache.blocks[idx].ref_count == 0) {
                g_kv_cache.blocks[idx].state = KV_BLOCK_FREE;
                g_kv_cache.blocks[idx].owner = AGENT_ID_INVALID;
            }
        }
    }
    
    g_kv_cache.used_size -= range->size;
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    return 0;
}

int kv_read(struct kv_range *range, void *buffer, uint32_t buffer_size) {
    if (!g_kv_cache.initialized || !range || !buffer) {
        return -1;
    }
    
    uint32_t read_size = 0;
    uint8_t *buf = (uint8_t *)buffer;
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    /* 逐块读取数据 */
    for (uint32_t i = 0; i < range->count && read_size < buffer_size; i++) {
        uint32_t idx = range->start - 1 + i;
        if (idx >= g_kv_cache.block_count) {
            break;
        }
        
        /* 跳过空闲块 */
        if (g_kv_cache.blocks[idx].state != KV_BLOCK_FREE) {
            /* 计算本次拷贝大小（不超过块大小和缓冲区剩余空间） */
            uint32_t copy_size = g_kv_cache.block_size;
            if (read_size + copy_size > buffer_size) {
                copy_size = buffer_size - read_size;
            }
            
            memcpy(buf + read_size, g_kv_cache.blocks[idx].data, copy_size);
            read_size += copy_size;
            
            /* 更新访问时间和统计 */
            g_kv_cache.blocks[idx].last_access = get_time_ms();
            g_kv_cache.stats.hit_count++;
        }
    }
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    return read_size;
}

int kv_write(struct kv_range *range, const void *data, uint32_t data_size) {
    if (!g_kv_cache.initialized || !range || !data) {
        return -1;
    }
    
    uint32_t written_size = 0;
    const uint8_t *src = (const uint8_t *)data;
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    /* 逐块写入数据 */
    for (uint32_t i = 0; i < range->count && written_size < data_size; i++) {
        uint32_t idx = range->start - 1 + i;
        if (idx >= g_kv_cache.block_count) {
            break;
        }
        
        if (g_kv_cache.blocks[idx].state != KV_BLOCK_FREE) {
            uint32_t copy_size = g_kv_cache.block_size;
            if (written_size + copy_size > data_size) {
                copy_size = data_size - written_size;
            }
            
            memcpy(g_kv_cache.blocks[idx].data, src + written_size, copy_size);
            written_size += copy_size;
            
            g_kv_cache.blocks[idx].last_access = get_time_ms();
        }
    }
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    return written_size;
}

/* ========== 共享操作 ========== */

int kv_share(agent_id_t src, agent_id_t dst, struct kv_range *range) {
    if (!g_kv_cache.initialized || !range) {
        return -1;
    }
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    /* 遍历范围中的每个块 */
    for (uint32_t i = 0; i < range->count; i++) {
        uint32_t idx = range->start - 1 + i;
        if (idx >= g_kv_cache.block_count) {
            pthread_mutex_unlock(&g_kv_cache.lock);
            return -1;
        }
        
        /* 确认块属于源Agent */
        if (g_kv_cache.blocks[idx].owner == src) {
            /* 状态改为SHARED，引用计数递增 */
            g_kv_cache.blocks[idx].state = KV_BLOCK_SHARED;
            g_kv_cache.blocks[idx].ref_count++;
        }
    }
    
    /* 更新共享统计 */
    g_kv_cache.stats.total_shared += range->size;
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    return 0;
}

int kv_unshare(agent_id_t id, struct kv_range *range) {
    if (!g_kv_cache.initialized || !range) {
        return -1;
    }
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    for (uint32_t i = 0; i < range->count; i++) {
        uint32_t idx = range->start - 1 + i;
        if (idx >= g_kv_cache.block_count) {
            continue;  /* 跳过无效块 */
        }
        
        /* 递减引用计数 */
        if (g_kv_cache.blocks[idx].ref_count > 0) {
            g_kv_cache.blocks[idx].ref_count--;
            /* 仅剩1个引用时恢复为普通分配状态 */
            if (g_kv_cache.blocks[idx].ref_count == 1) {
                g_kv_cache.blocks[idx].state = KV_BLOCK_ALLOCATED;
            }
        }
    }
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    return 0;
}

/* ========== 压缩操作 ========== */

int kv_compress(struct kv_range *input, float ratio, struct kv_range *output) {
    /* 参数校验：压缩比必须在(0, 1)范围内 */
    if (!g_kv_cache.initialized || !input || !output || ratio <= 0.0f || ratio >= 1.0f) {
        return -1;
    }
    
    /* 计算压缩后的块数量 */
    uint32_t output_blocks = (uint32_t)(input->count * ratio);
    if (output_blocks == 0) {
        output_blocks = 1;  /* 至少保留1个块 */
    }
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    /* 为压缩结果分配空间 */
    kv_block_id_t out_start = find_free_block(output_blocks);
    if (out_start == 0) {
        pthread_mutex_unlock(&g_kv_cache.lock);
        return -1;  /* 空间不足 */
    }
    
    output->start = out_start;
    output->count = output_blocks;
    output->size = output_blocks * g_kv_cache.block_size;
    
    /* 按比例均匀采样，从输入块中选取代表性数据 */
    for (uint32_t i = 0; i < output_blocks; i++) {
        /* 均匀采样：计算源块索引 */
        uint32_t src_idx = (input->start - 1) + (i * input->count / output_blocks);
        uint32_t dst_idx = out_start - 1 + i;
        
        if (src_idx < g_kv_cache.block_count && dst_idx < g_kv_cache.block_count) {
            /* 复制采样数据 */
            memcpy(g_kv_cache.blocks[dst_idx].data,
                   g_kv_cache.blocks[src_idx].data,
                   g_kv_cache.block_size);
            
            /* 标记为压缩状态 */
            g_kv_cache.blocks[dst_idx].state = KV_BLOCK_COMPRESSED;
            g_kv_cache.blocks[dst_idx].ref_count = 1;
            g_kv_cache.blocks[dst_idx].owner = g_kv_cache.blocks[src_idx].owner;
            g_kv_cache.blocks[dst_idx].last_access = get_time_ms();
        }
    }
    
    /* 更新压缩统计 */
    g_kv_cache.stats.total_compressed += output->size;
    if (g_kv_cache.stats.total_allocated > 0) {
        g_kv_cache.stats.compress_ratio = 
            (float)g_kv_cache.stats.total_compressed / g_kv_cache.stats.total_allocated;
    }
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    return 0;
}

int kv_decompress(struct kv_range *compressed, struct kv_range *output) {
    if (!g_kv_cache.initialized || !compressed || !output) {
        return -1;
    }
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    /* 将压缩块恢复为普通分配状态 */
    for (uint32_t i = 0; i < compressed->count; i++) {
        uint32_t idx = compressed->start - 1 + i;
        if (idx < g_kv_cache.block_count) {
            g_kv_cache.blocks[idx].state = KV_BLOCK_ALLOCATED;
        }
    }
    
    output->start = compressed->start;
    output->count = compressed->count;
    output->size = compressed->size;
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    return 0;
}

/* ========== 复用操作 ========== */

int kv_reuse_prefix(agent_id_t agent, const void *prefix, uint32_t prefix_size,
                    struct kv_range *range) {
    if (!g_kv_cache.initialized || !prefix || !range) {
        return -1;
    }
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    /* 遍历所有块，查找属于该Agent且内容匹配的块 */
    for (uint32_t i = 0; i < g_kv_cache.block_count; i++) {
        if (g_kv_cache.blocks[i].state != KV_BLOCK_FREE &&
            g_kv_cache.blocks[i].owner == agent) {
            
            /* 比较块内容与给定前缀 */
            uint32_t match_size = 0;
            uint32_t check_size = prefix_size < g_kv_cache.block_size ? 
                                  prefix_size : g_kv_cache.block_size;
            
            if (memcmp(g_kv_cache.blocks[i].data, prefix, check_size) == 0) {
                match_size = check_size;
            }
            
            if (match_size > 0) {
                /* 找到匹配，返回该块的引用 */
                range->start = g_kv_cache.blocks[i].id;
                range->count = 1;
                range->size = g_kv_cache.block_size;
                
                /* 增加引用计数，更新访问时间 */
                g_kv_cache.blocks[i].ref_count++;
                g_kv_cache.blocks[i].last_access = get_time_ms();
                g_kv_cache.stats.reuse_count++;
                
                pthread_mutex_unlock(&g_kv_cache.lock);
                return 0;  /* 复用成功 */
            }
        }
    }
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    return -1;  /* 未找到可复用的前缀 */
}

/* ========== 统计 ========== */

int kv_get_stats(struct kv_stats *stats) {
    if (!g_kv_cache.initialized || !stats) {
        return -1;
    }
    
    pthread_mutex_lock(&g_kv_cache.lock);
    
    /* 拷贝统计信息 */
    memcpy(stats, &g_kv_cache.stats, sizeof(struct kv_stats));
    
    /* 计算命中率 */
    uint64_t total_access = stats->hit_count + stats->miss_count;
    if (total_access > 0) {
        stats->hit_rate = (float)stats->hit_count / total_access;
    }
    
    pthread_mutex_unlock(&g_kv_cache.lock);
    return 0;
}