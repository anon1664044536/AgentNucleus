/**
 * @file kv_cache.h
 * @brief KV Cache管理模块 - 头文件
 * 
 * 实现多Agent系统中KV Cache的统一管理，核心功能包括：
 *   - 显存池化管理：将大块显存分割为固定大小的块进行管理
 *   - 共享机制：多个Agent可以安全地共享同一份KV Cache（引用计数）
 *   - 复用机制：检测并复用相同前缀的上下文，避免重复计算
 *   - 压缩机制：按比例压缩KV Cache，降低显存占用
 *   - 隔离机制：通过引用计数和状态管理实现Agent间的缓存隔离
 */

#ifndef KV_CACHE_H
#define KV_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include "agent.h"

/* ===== 常量定义 ===== */
#define KV_BLOCK_SIZE_DEFAULT   (4096)           /* 默认块大小：4KB */
#define KV_MAX_BLOCKS           (1024 * 1024)    /* 最大块数：1M块 */

/* ===== 类型定义 ===== */
typedef uint64_t kv_block_id_t;   /* KV Cache块ID类型 */

/**
 * @brief KV Cache块状态
 * 
 * 状态转换：
 *   FREE → ALLOCATED → SHARED → FREE（引用计数归零时）
 *   ALLOCATED → COMPRESSED → ALLOCATED（解压缩时）
 */
enum kv_block_state {
    KV_BLOCK_FREE = 0,          /* 空闲块，可被分配 */
    KV_BLOCK_ALLOCATED,         /* 已分配，属于某个Agent */
    KV_BLOCK_SHARED,            /* 共享状态，被多个Agent引用 */
    KV_BLOCK_COMPRESSED,        /* 已压缩状态 */
    KV_BLOCK_MAX                /* 状态枚举上限 */
};

/**
 * @brief KV Cache块描述符
 * 
 * 每个块描述符对应一个固定大小的显存块，记录其状态、
 * 引用计数、拥有者等元信息。
 */
struct kv_block {
    kv_block_id_t id;           /* 块的唯一ID（从1开始） */
    uint32_t size;              /* 块大小（字节） */
    enum kv_block_state state;  /* 当前状态 */
    uint32_t ref_count;         /* 引用计数（多个Agent共享时递增） */
    agent_id_t owner;           /* 拥有者Agent ID */
    uint64_t last_access;       /* 最后访问时间（毫秒），用于LRU淘汰 */
    uint32_t priority;          /* 优先级（高优先级块不易被替换） */
    void *data;                 /* 指向实际数据的指针 */
};

/**
 * @brief KV Cache范围描述
 * 
 * 描述一段连续的KV Cache块区间。
 */
struct kv_range {
    kv_block_id_t start;        /* 起始块ID */
    uint32_t count;             /* 包含的块数量 */
    uint32_t size;              /* 总大小（字节） */
};

/**
 * @brief KV Cache配置
 */
struct kv_config {
    uint64_t total_size;        /* 总显存大小（字节），0表示自动计算 */
    uint32_t block_size;        /* 块大小（字节），0使用默认值4KB */
    uint32_t max_blocks;        /* 最大块数，0使用默认值 */
    float compress_ratio;       /* 压缩比（0.0~1.0），如0.5表示压缩到50% */
    bool enable_reuse;          /* 是否启用上下文复用 */
    bool enable_shared;         /* 是否启用多Agent共享 */
};

/**
 * @brief KV Cache统计信息
 */
struct kv_stats {
    uint64_t total_allocated;   /* 累计分配的总字节数 */
    uint64_t total_shared;      /* 累计共享的总字节数 */
    uint64_t total_compressed;  /* 累计压缩的总字节数 */
    uint64_t hit_count;         /* 缓存命中次数 */
    uint64_t miss_count;        /* 缓存未命中次数 */
    uint64_t reuse_count;       /* 上下文复用次数 */
    float hit_rate;             /* 命中率 = hit / (hit + miss) */
    float compress_ratio;       /* 当前压缩比 */
};

/* ===== 初始化与销毁 ===== */

/** 初始化KV Cache管理器 */
int kv_cache_init(const struct kv_config *config);
/** 销毁KV Cache管理器，释放所有显存 */
void kv_cache_destroy(void);

/* ===== 基本操作 ===== */

/**
 * @brief 为Agent分配KV Cache空间
 * @param agent 拥有者Agent ID
 * @param size 需要的字节数（会向上对齐到块大小的整数倍）
 * @param range [out] 输出分配的范围
 * @return 成功返回0，空间不足返回-1
 */
int kv_allocate(agent_id_t agent, uint32_t size, struct kv_range *range);

/**
 * @brief 释放KV Cache空间
 * @param range 要释放的范围
 * @note 如果块被共享，仅减少引用计数；引用计数归零时才真正释放
 */
int kv_free(struct kv_range *range);

/**
 * @brief 从KV Cache读取数据
 * @param range 要读取的范围
 * @param buffer 目标缓冲区
 * @param buffer_size 缓冲区大小
 * @return 实际读取的字节数
 */
int kv_read(struct kv_range *range, void *buffer, uint32_t buffer_size);

/**
 * @brief 向KV Cache写入数据
 * @param range 要写入的范围
 * @param data 源数据
 * @param data_size 数据大小
 * @return 实际写入的字节数
 */
int kv_write(struct kv_range *range, const void *data, uint32_t data_size);

/* ===== 共享操作 ===== */

/**
 * @brief 将Agent的KV Cache共享给另一个Agent
 * @param src 源Agent（拥有者）
 * @param dst 目标Agent（共享者）
 * @param range 要共享的范围
 * @note 共享后引用计数递增，状态变为SHARED
 */
int kv_share(agent_id_t src, agent_id_t dst, struct kv_range *range);

/**
 * @brief 取消Agent对KV Cache的共享
 * @param id 要取消共享的Agent
 * @param range 要取消的范围
 * @note 引用计数递减，当仅剩1个引用时状态恢复为ALLOCATED
 */
int kv_unshare(agent_id_t id, struct kv_range *range);

/* ===== 压缩操作 ===== */

/**
 * @brief 压缩KV Cache
 * @param input 输入范围
 * @param ratio 压缩比（如0.5表示压缩到原来的一半）
 * @param output [out] 输出压缩后的范围
 * @return 成功返回0
 */
int kv_compress(struct kv_range *input, float ratio, struct kv_range *output);

/**
 * @brief 解压缩KV Cache
 * @param compressed 压缩后的范围
 * @param output [out] 输出解压后的范围
 */
int kv_decompress(struct kv_range *compressed, struct kv_range *output);

/* ===== 复用操作 ===== */

/**
 * @brief 尝试复用已有的KV Cache前缀
 * @param agent Agent ID
 * @param prefix 要匹配的前缀数据
 * @param prefix_size 前缀大小
 * @param range [out] 匹配到的范围
 * @return 匹配成功返回0，未找到匹配返回-1
 * @note 用于避免重复计算相同prompt前缀的KV Cache
 */
int kv_reuse_prefix(agent_id_t agent, const void *prefix, uint32_t prefix_size,
                    struct kv_range *range);

/* ===== 统计 ===== */

/** 获取KV Cache统计信息 */
int kv_get_stats(struct kv_stats *stats);

#endif /* KV_CACHE_H */