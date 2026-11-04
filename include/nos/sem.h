/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 32 位计数信号量 (nos_sem) 与通用等待底座集成
 */

#ifndef __NOS_IPC_SEM_H__
#define __NOS_IPC_SEM_H__

#include <stdint.h>
#include <nos/object.h>
#include <nos/wait.h>
#include <nos/timeout.h>

/**
 * 32 位计数信号量结构体
 */
struct nos_sem {
    struct nos_object     object;      /* 通用对象生命周期管理头 (包含对象自旋锁与引用计数) */
    uint32_t              count;       /* 当前可用资源令牌数 */
    uint32_t              limit;       /* 最大允许的令牌上限 */
    struct nos_wait_queue wait_queue;  /* 按任务实时优先级排队的等待队列 */
};

typedef struct nos_sem nos_sem_t;

/**
 * 初始化 32 位计数信号量
 *
 * @param sem: 待初始化的信号量指针
 * @param initial: 初始可用令牌数
 * @param limit: 允许的最大令牌上限 (必须 > 0，且 initial <= limit)
 * @return: 0 成功，-EINVAL 参数非法
 */
int nos_sem_init(struct nos_sem *sem, uint32_t initial, uint32_t limit);

/**
 * 获取信号量令牌 (带超时)
 *
 * @param sem: 目标信号量指针
 * @param timeout: 超时设置 (NOS_NO_WAIT, NOS_WAIT_FOREVER 或绝对 ticks)
 * @return: 0 成功获取令牌，-EINVAL 参数非法，-EAGAIN 令牌不足(非阻塞)，
 *          -ETIMEDOUT 等待超时，-EPIPE 信号量已被关闭
 */
int nos_sem_take(struct nos_sem *sem, nos_timeout_t timeout);

/**
 * 非阻塞尝试获取信号量令牌 (等价于 nos_sem_take(sem, NOS_NO_WAIT))
 *
 * @param sem: 目标信号量指针
 * @return: 0 成功获取令牌，-EINVAL 参数非法，-EAGAIN 令牌不足，-EPIPE 信号量已被关闭
 */
int nos_sem_trytake(struct nos_sem *sem);

/**
 * 释放信号量令牌
 *
 * 若有任务在等待队列中，直接将令牌交接给最高优先级的等待任务并加入延后唤醒批次 (count 不变)；
 * 若无任务等待且 count < limit，则累加 count++；
 * 若无任务等待且 count == limit，返回 -EOVERFLOW 拒绝静默饱和溢出。
 *
 * @param sem: 目标信号量指针
 * @return: 0 成功，-EINVAL 参数非法，-EOVERFLOW 超过 limit 上限，-EPIPE 信号量已被关闭
 */
int nos_sem_give(struct nos_sem *sem);

/**
 * 关闭信号量
 *
 * 将对象标记为已关闭，并唤醒当前等待队列中的所有任务返回 -EPIPE。
 * 具有幂等性，关闭后禁止接纳新获取与释放操作。
 *
 * @param sem: 目标信号量指针
 * @return: 0 成功，-EINVAL 参数非法
 */
int nos_sem_close(struct nos_sem *sem);

#endif /* __NOS_IPC_SEM_H__ */
