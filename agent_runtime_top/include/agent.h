/**
 * @file agent.h
 * @brief Agent执行抽象模型 - 头文件
 * 
 * 定义了Agent的核心数据结构、状态机和生命周期管理接口。
 * Agent是操作系统中的一等公民，由Runtime统一管理其创建、调度和销毁。
 */

#ifndef AGENT_RUNTIME_H
#define AGENT_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>

/* ===== 常量定义 ===== */
#define AGENT_ID_INVALID    0       /* 无效的Agent ID（表示未分配） */
#define AGENT_NAME_MAX      64      /* Agent名称最大长度 */
#define TASK_QUEUE_SIZE     128     /* 任务队列最大容量 */

/* ===== 类型定义 ===== */
typedef uint64_t agent_id_t;        /* Agent唯一标识符类型 */
typedef uint64_t task_id_t;         /* 任务唯一标识符类型 */

/**
 * @brief Agent状态枚举
 * 
 * 状态转换规则：
 *   CREATED → READY → RUNNING → COMPLETED
 *                ↑       ↓
 *                └───────┘（调度/等待循环）
 *   任何状态 → ERROR（异常时）
 */
enum agent_state {
    AGENT_STATE_CREATED = 0,    /* 已创建，未初始化 */
    AGENT_STATE_READY,          /* 已初始化，等待被调度执行 */
    AGENT_STATE_RUNNING,        /* 正在执行中 */
    AGENT_STATE_WAITING,        /* 等待外部资源（如模型响应） */
    AGENT_STATE_COMPLETED,      /* 任务执行完成 */
    AGENT_STATE_ERROR,          /* 执行出错 */
    AGENT_STATE_MAX             /* 状态枚举上限（用于边界检查） */
};

/**
 * @brief Agent配置结构体
 * 
 * 在创建Agent时使用，定义Agent的基本属性。
 */
struct agent_config {
    char name[AGENT_NAME_MAX];  /* Agent名称（如 "Planner"、"Coder"） */
    uint32_t priority;          /* 调度优先级（数值越大优先级越高） */
    uint64_t memory_limit;      /* 内存使用上限（字节） */
    uint32_t cpu_affinity;      /* CPU亲和性掩码（位图，如 0x01 表示核心0） */
};

/**
 * @brief Agent操作函数集
 * 
 * 通过函数指针实现多态，不同类型的Agent可以注册不同的操作实现。
 */
struct agent_ops {
    /** 初始化回调：分配资源、加载配置等 */
    int (*init)(void *ctx);
    /** 执行回调：执行具体任务 */
    int (*execute)(void *ctx, void *task);
    /** 清理回调：释放资源 */
    int (*cleanup)(void *ctx);
    /** 错误回调：处理执行异常 */
    void (*on_error)(void *ctx, int error_code);
};

/**
 * @brief Agent核心结构体
 * 
 * 描述一个Agent的完整运行时状态，包括身份信息、状态、
 * 优先级、上下文和操作接口。
 */
struct agent {
    agent_id_t id;              /* 唯一标识符 */
    char name[AGENT_NAME_MAX];  /* Agent名称 */
    enum agent_state state;     /* 当前状态 */
    uint32_t priority;          /* 调度优先级 */
    void *context;              /* 私有上下文数据（由ops管理） */
    struct agent_ops *ops;      /* 操作函数集 */
    uint64_t create_time;       /* 创建时间戳（毫秒） */
    uint64_t last_active;       /* 最后活跃时间戳（毫秒） */
};

/* ===== Runtime全局管理接口 ===== */

/** 初始化Agent Runtime系统（必须在使用其他接口前调用） */
int agent_runtime_init(void);
/** 销毁Agent Runtime系统，释放所有资源 */
void agent_runtime_destroy(void);

/* ===== Agent生命周期管理接口 ===== */

/**
 * @brief 创建一个Agent
 * @param config Agent配置参数
 * @return 成功返回Agent ID，失败返回AGENT_ID_INVALID
 */
agent_id_t agent_create(const struct agent_config *config);

/**
 * @brief 销毁一个Agent
 * @param id 要销毁的Agent ID
 * @return 成功返回0，失败返回-1
 */
int agent_destroy(agent_id_t id);

/**
 * @brief 初始化Agent（调用ops->init）
 * @param id Agent ID
 * @return 成功返回0，失败返回-1
 */
int agent_init(agent_id_t id);

/**
 * @brief 启动Agent执行（状态: READY → RUNNING）
 * @param id Agent ID
 * @return 成功返回0，失败返回-1
 */
int agent_start(agent_id_t id);

/**
 * @brief 停止Agent执行（状态: RUNNING → COMPLETED）
 * @param id Agent ID
 * @return 成功返回0，失败返回-1
 */
int agent_stop(agent_id_t id);

/**
 * @brief 等待Agent执行完成
 * @param id Agent ID
 * @param timeout_ms 超时时间（毫秒），0表示无限等待
 * @return 成功返回0，超时返回-1
 */
int agent_wait(agent_id_t id, int timeout_ms);

/* ===== 查询与设置接口 ===== */

/** 获取Agent当前状态 */
enum agent_state agent_get_state(agent_id_t id);

/** 将状态枚举转换为可读字符串 */
const char *agent_state_str(enum agent_state state);

/** 设置Agent优先级 */
int agent_set_priority(agent_id_t id, uint32_t priority);

#endif /* AGENT_RUNTIME_H */