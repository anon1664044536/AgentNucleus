/**
 * @file resource.h
 * @brief 资源抽象层 - 头文件
 * 
 * 提供统一的资源管理接口，将模型调用、工具调用与底层计算资源
 * （CPU、GPU、内存、网络等）抽象为统一的资源模型。
 * 
 * 核心设计：
 *   - 资源描述符（resource_desc）：描述资源的元信息
 *   - 资源操作集（resource_ops）：定义资源的探针、分配、释放等操作
 *   - 资源句柄（resource_handle）：持有已分配资源的引用
 *   - 资源池（隐式）：通过注册/注销机制管理所有资源
 */

#ifndef RESOURCE_H
#define RESOURCE_H

#include <stdint.h>
#include <stdbool.h>

/* ===== 类型定义 ===== */
typedef uint64_t resource_id_t;  /* 资源唯一标识符类型 */

/**
 * @brief 资源类型枚举
 */
enum resource_type {
    RESOURCE_TYPE_MODEL = 0,    /* AI模型资源（LLM、Embedding等） */
    RESOURCE_TYPE_TOOL,         /* 工具资源（代码执行、文件系统等） */
    RESOURCE_TYPE_CPU,          /* CPU计算资源 */
    RESOURCE_TYPE_GPU,          /* GPU计算资源 */
    RESOURCE_TYPE_MEMORY,       /* 内存资源 */
    RESOURCE_TYPE_NETWORK,      /* 网络资源 */
    RESOURCE_TYPE_MAX           /* 类型枚举上限 */
};

/**
 * @brief 资源状态枚举
 */
enum resource_state {
    RESOURCE_STATE_IDLE = 0,    /* 空闲，可被分配 */
    RESOURCE_STATE_BUSY,        /* 忙碌，已被占用 */
    RESOURCE_STATE_ERROR,       /* 错误状态，不可用 */
    RESOURCE_STATE_OFFLINE,     /* 离线，物理设备不可达 */
    RESOURCE_STATE_MAX          /* 状态枚举上限 */
};

/**
 * @brief 资源描述符
 * 
 * 描述一个资源的完整元信息，包括ID、名称、类型、状态和容量。
 */
struct resource_desc {
    resource_id_t id;           /* 资源唯一ID */
    char name[64];              /* 资源名称（如 "GPU_0"、"LLM_Model_A"） */
    enum resource_type type;    /* 资源类型 */
    enum resource_state state;  /* 当前状态 */
    uint64_t capacity;          /* 总容量（根据类型不同，单位可能是字节/算力等） */
    uint64_t used;              /* 已使用容量 */
    void *private_data;         /* 私有数据（由具体资源实现使用） */
};

/**
 * @brief 资源操作集
 * 
 * 定义了资源的生命周期操作，采用函数指针实现多态。
 * 不同类型的资源可以实现不同的操作集。
 */
struct resource_ops {
    /** 探测：检测物理设备是否存在 */
    int (*probe)(void);
    /** 初始化：设置资源初始状态 */
    int (*init)(struct resource_desc *desc);
    /** 销毁：释放资源相关资源 */
    int (*destroy)(struct resource_desc *desc);
    /** 分配：从资源中分配指定大小 */
    int (*allocate)(struct resource_desc *desc, uint64_t size);
    /** 释放：归还指定大小到资源 */
    int (*release)(struct resource_desc *desc, uint64_t size);
    /** 获取统计信息 */
    int (*get_stats)(struct resource_desc *desc, void *stats);
};

/**
 * @brief 资源句柄
 * 
 * 通过resource_allocate获得，持有已分配资源的引用，
 * 用于后续操作和释放。
 */
struct resource_handle {
    resource_id_t id;           /* 资源ID */
    struct resource_desc *desc; /* 指向资源描述符 */
    void *context;              /* 使用上下文 */
    uint64_t allocated_size;    /* 已分配的大小 */
};

/**
 * @brief 资源分配请求
 */
struct resource_request {
    enum resource_type type;    /* 请求的资源类型 */
    uint64_t size;              /* 需要的大小 */
    uint32_t priority;          /* 请求优先级 */
    void *params;               /* 附加参数 */
};

/* ===== 初始化与销毁 ===== */

/** 初始化资源管理器 */
int resource_init(void);
/** 销毁资源管理器，释放所有资源 */
void resource_destroy(void);

/* ===== 资源注册 ===== */

/**
 * @brief 注册一个资源
 * @param desc 资源描述符
 * @param ops 资源操作集
 */
int resource_register(struct resource_desc *desc, struct resource_ops *ops);

/** 注销一个资源 */
int resource_unregister(resource_id_t id);

/* ===== 资源发现 ===== */

/**
 * @brief 发现指定类型的所有资源
 * @param type 资源类型
 * @param descs [out] 输出资源描述符数组（调用者需free）
 * @param count [out] 输出资源数量
 * @return 成功返回0
 */
int resource_discover(enum resource_type type, 
                      struct resource_desc **descs,
                      uint32_t *count);

/* ===== 资源分配与释放 ===== */

/**
 * @brief 分配资源
 * @param request 分配请求
 * @param handle [out] 输出资源句柄
 * @return 成功返回0，无可用资源返回-1
 */
int resource_allocate(const struct resource_request *request,
                      struct resource_handle *handle);

/** 释放资源（通过句柄） */
int resource_release(struct resource_handle *handle);

/* ===== 查询 ===== */

/** 获取指定ID的资源信息 */
int resource_get_info(resource_id_t id, struct resource_desc *desc);

#endif /* RESOURCE_H */