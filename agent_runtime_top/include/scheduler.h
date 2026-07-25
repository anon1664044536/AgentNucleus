/**
 * @file scheduler.h
 * @brief Agent调度器 - 头文件
 * 
 * 实现多Agent任务的统一调度，支持FIFO、时间片轮转(RR)和优先级调度策略。
 * 调度器维护一个就绪队列，根据策略选择下一个执行的Agent。
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "agent.h"

/**
 * @brief 调度器配置结构体
 */
struct scheduler_config {
    uint32_t max_agents;        /* 调度队列最大容量 */
    uint32_t time_slice_ms;     /* 时间片大小（毫秒），仅RR模式使用 */
    bool preemptive;            /* 是否启用抢占式调度 */
    enum {
        SCHED_FIFO,             /* 先来先服务：按到达顺序调度 */
        SCHED_RR,               /* 时间片轮转：每个Agent执行固定时间片后切换 */
        SCHED_PRIORITY          /* 优先级调度：高优先级Agent优先执行 */
    } policy;                   /* 调度策略 */
};

/**
 * @brief 调度器统计信息
 */
struct scheduler_stats {
    uint64_t total_schedules;   /* 总调度次数 */
    uint64_t context_switches;  /* 上下文切换次数 */
    uint64_t idle_time;         /* 空闲时间（毫秒） */
    uint64_t busy_time;         /* 忙碌时间（毫秒） */
};

/* ===== 初始化与销毁 ===== */

/** 初始化调度器 */
int scheduler_init(const struct scheduler_config *config);
/** 销毁调度器 */
void scheduler_destroy(void);

/* ===== Agent队列管理 ===== */

/** 将Agent加入就绪队列 */
int scheduler_add_agent(agent_id_t id);
/** 从就绪队列中移除Agent */
int scheduler_remove_agent(agent_id_t id);
/** 更新Agent在队列中的位置（提升优先级到队首） */
int scheduler_update_agent(agent_id_t id);

/* ===== 调度操作 ===== */

/**
 * @brief 选择下一个执行的Agent
 * @return 返回被选中的Agent ID，队列为空返回0
 * @note 如果队列为空且调度器正在运行，会阻塞等待直到有Agent入队
 */
agent_id_t scheduler_pick_next(void);

/** Agent主动让出CPU（将自身重新加入就绪队列尾部） */
int scheduler_yield(agent_id_t id);

/** 阻塞Agent（从就绪队列移除，等待唤醒） */
int scheduler_block(agent_id_t id);

/** 唤醒被阻塞的Agent（重新加入就绪队列） */
int scheduler_unblock(agent_id_t id);

/* ===== 调度器控制 ===== */

/** 启动调度器 */
int scheduler_start(void);
/** 停止调度器 */
int scheduler_stop(void);

/** 获取调度器统计信息 */
int scheduler_get_stats(struct scheduler_stats *stats);

#endif /* SCHEDULER_H */