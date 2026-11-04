/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 完成量 (nos_completion) 核心接口定义
 */

#ifndef __NOS_IPC_COMPLETION_H__
#define __NOS_IPC_COMPLETION_H__

#include <stdint.h>
#include <nos/object.h>
#include <nos/wait.h>
#include <nos/timeout.h>

/**
 * 完成量结构体
 */
struct nos_completion {
    struct nos_object     object;      /* 通用对象生命周期管理头 (包含对象自旋锁与引用计数) */
    uint32_t              done;        /* 0: 未完成; >0: 单次完成计数; UINT32_MAX: 广播完成 */
    struct nos_wait_queue wait_queue;  /* 等待队列 (FIFO 次序) */
};

typedef struct nos_completion nos_completion_t;

/**
 * 初始化完成量
 *
 * @param comp: 待初始化的完成量指针
 * @return: 0 成功，-EINVAL 参数非法
 */
int nos_completion_init(struct nos_completion *comp);

/**
 * 等待完成量 (带超时)
 *
 * @param comp: 目标完成量指针
 * @param timeout: 超时设置 (NOS_NO_WAIT, NOS_WAIT_FOREVER 或绝对 ticks)
 * @return: 0 成功完成，-EINVAL 参数非法，-EAGAIN 未完成(非阻塞)，
 *          -ETIMEDOUT 等待超时，-EPIPE 完成量已被关闭
 */
int nos_completion_wait(struct nos_completion *comp, nos_timeout_t timeout);

/**
 * 单次完成：唤醒一个等待者，若无等待者则累加完成计数
 *
 * @param comp: 目标完成量指针
 * @return: 0 成功，-EINVAL 参数非法，-EPIPE 完成量已被关闭
 */
int nos_complete(struct nos_completion *comp);

/**
 * 广播完成：唤醒所有活动等待者，并将状态置为广播完成 (后续 wait 立即无阻塞返回 0)
 *
 * @param comp: 目标完成量指针
 * @return: 0 成功，-EINVAL 参数非法，-EPIPE 完成量已被关闭
 */
int nos_complete_all(struct nos_completion *comp);

/**
 * 重置完成量状态 (仅在无活动等待者时允许重置为 0)
 *
 * @param comp: 目标完成量指针
 * @return: 0 成功，-EINVAL 参数非法，-EBUSY 仍有等待者排队，-EPIPE 完成量已被关闭
 */
int nos_completion_reset(struct nos_completion *comp);

/**
 * 关闭完成量并唤醒所有等待者 (返回 -EPIPE)
 *
 * @param comp: 目标完成量指针
 * @return: 0 成功，-EINVAL 参数非法
 */
int nos_completion_close(struct nos_completion *comp);

#endif /* __NOS_IPC_COMPLETION_H__ */
