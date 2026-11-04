/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 通用等待底座 (nos_wait_entry, nos_wait_queue)
 * 与单点仲裁状态机、延后唤醒批次 (nos_wake_batch)。
 */

#ifndef __NOS_WAIT_H__
#define __NOS_WAIT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/list.h>
#include <nos/object.h>
#include <nos/timeout.h>

struct task_struct;

/**
 * 等待记录状态枚举 (单点状态机)
 */
enum nos_wait_state {
    NOS_WAIT_STATE_IDLE = 0,       /* 空闲状态：无等待任务进行中 */
    NOS_WAIT_STATE_WAITING,        /* 等待中：已挂入对象等待队列 */
    NOS_WAIT_STATE_SUCCEEDED,      /* 成功完成：资源已原子交付 */
    NOS_WAIT_STATE_TIMEDOUT,       /* 超时完成：单调截止时间已过期 */
    NOS_WAIT_STATE_CLOSED,         /* 关闭完成：等待对象已被关闭 */
    NOS_WAIT_STATE_CANCELLED,      /* 取消完成：操作被主动取消 */
};

/**
 * 通用等待记录结构体 (内嵌在 struct task_struct 中)
 */
typedef struct nos_wait_entry {
    struct list_head       wait_node;     /* 挂入等待对象的队列节点 */
    struct list_head       wake_node;     /* 延后唤醒批次链表节点 */
    uint64_t               epoch;         /* 严格单调递增的等待代数 */
    uint8_t                state;         /* enum nos_wait_state 状态 */
    int32_t                result;        /* 最终完成返回值 */
    uint64_t               deadline;      /* 绝对单调截止时间戳 (ticks) */
    struct nos_object     *waiting_obj;   /* 当前等待的对象引用 */
    struct nos_wait_queue *queue;         /* 所挂载的等待队列指针 */
    struct task_struct    *task;          /* 所属任务反向指针 */
} nos_wait_entry_t;

/**
 * 通用等待队列结构体
 */
typedef struct nos_wait_queue {
    struct list_head       list;          /* 等待者链表头 */
    uint32_t               count;         /* 当前排队的等待者数量 */
} nos_wait_queue_t;

/**
 * 延后调度唤醒批次结构体
 */
typedef struct nos_wake_batch {
    struct list_head       list;          /* 待唤醒任务链表头 (wake_node 串联) */
    uint32_t               count;         /* 批次中待唤醒的任务数 */
} nos_wake_batch_t;

/**
 * 初始化等待队列
 */
static inline void nos_wait_queue_init(struct nos_wait_queue *wq)
{
    if (wq != NULL) {
        INIT_LIST_HEAD(&wq->list);
        wq->count = 0U;
    }
}

/**
 * 查询等待队列是否为空
 */
static inline bool nos_wait_queue_empty(const struct nos_wait_queue *wq)
{
    return (wq == NULL) || list_empty(&wq->list);
}

/**
 * 查询等待队列中的任务数
 */
static inline uint32_t nos_wait_queue_count(const struct nos_wait_queue *wq)
{
    return wq ? wq->count : 0U;
}

/**
 * 获取等待队列的第一个等待项
 */
static inline struct nos_wait_entry *nos_wait_queue_first(const struct nos_wait_queue *wq)
{
    if (wq == NULL || list_empty(&wq->list)) {
        return NULL;
    }
    return list_first_entry(&wq->list, struct nos_wait_entry, wait_node);
}

/**
 * 将等待项按 FIFO 规则加入队列末尾
 */
void nos_wait_queue_add_tail(struct nos_wait_queue *wq, struct nos_wait_entry *entry);

/**
 * 将等待项按任务实时优先级排序加入队列 (同级 FIFO，数值小优先级高)
 */
void nos_wait_queue_add_prio(struct nos_wait_queue *wq, struct nos_wait_entry *entry);

/**
 * 从等待队列中移除指定等待项
 */
void nos_wait_queue_del(struct nos_wait_queue *wq, struct nos_wait_entry *entry);

/**
 * 初始化等待记录
 *
 * @param entry: 待初始化的等待项指针
 * @param task: 所属的任务指针
 */
void nos_wait_entry_init(struct nos_wait_entry *entry, struct task_struct *task);

/**
 * 准备进入等待 (安装新代数、校验对象未关闭并持有对象引用)
 *
 * @param entry: 等待项指针
 * @param obj: 等待的目标对象
 * @param deadline: 绝对截止时间 (ticks)
 * @return: 0 成功，-EINVAL 参数错误，-EBUSY 重复等待，-EPIPE 对象已关闭，-EOVERFLOW 代数溢出
 */
int nos_wait_prepare(struct nos_wait_entry *entry, struct nos_object *obj, uint64_t deadline);

/**
 * 结束等待并重置为 IDLE 状态 (释放所持有的对象引用)
 *
 * @param entry: 等待项指针
 */
void nos_wait_finish(struct nos_wait_entry *entry);

/**
 * 持有对象锁时，单点原子完成等待裁决并转入指定终态
 *
 * @param entry: 等待项指针
 * @param state: 目标终态 (SUCCEEDED, TIMEDOUT, CLOSED, CANCELLED)
 * @param result: 最终返回值
 * @param batch: 延后唤醒批次指针 (可为 NULL)
 * @return: 0 成功，-EINVAL 参数错误，-EALREADY 非 WAITING 状态 (竞争裁决已发生)
 */
int nos_wait_complete_state_locked(struct nos_wait_entry *entry,
                                   enum nos_wait_state state,
                                   int32_t result,
                                   struct nos_wake_batch *batch);

/**
 * 持有对象锁时，根据返回值自动映射终态完成等待裁决 (单点线性化点)
 *
 * @param entry: 等待项指针
 * @param result: 完成返回值 (>=0 为 SUCCEEDED, -ETIMEDOUT 为 TIMEDOUT, -EPIPE 为 CLOSED, 其他为 CANCELLED)
 * @param batch: 延后唤醒批次指针 (可为 NULL)
 * @return: 0 成功，-EINVAL 参数错误，-EALREADY 竞争失败/已被其他路径完成
 */
int nos_wait_complete_locked(struct nos_wait_entry *entry,
                             int32_t result,
                             struct nos_wake_batch *batch);

/**
 * 初始化延后唤醒批次
 *
 * @param batch: 批次指针
 */
static inline void nos_wake_batch_init(struct nos_wake_batch *batch)
{
    if (batch != NULL) {
        INIT_LIST_HEAD(&batch->list);
        batch->count = 0U;
    }
}

/**
 * 向延后唤醒批次添加已完成裁决的等待项
 *
 * @param batch: 批次指针
 * @param entry: 等待项指针
 */
void nos_wake_batch_add(struct nos_wake_batch *batch, struct nos_wait_entry *entry);

/**
 * 在 object->lock 外分发延后唤醒批次
 * 逐个获取各个任务的 task->lock，设置可运行状态并加入就绪队列
 *
 * @param batch: 批次指针
 */
void nos_wake_batch_dispatch(struct nos_wake_batch *batch);

/**
 * 持有 task->lock 时的底层任务唤醒钩子
 *
 * @param task: 待唤醒的任务
 */
void nos_task_wake_up_locked(struct task_struct *task);

#endif /* __NOS_WAIT_H__ */
