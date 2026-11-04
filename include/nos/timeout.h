/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 通用超时机制与单调截止时间 (Deadline) 定义。
 */

#ifndef __NOS_TIMEOUT_H__
#define __NOS_TIMEOUT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/errno.h>

/**
 * 超时类型定义 (基于内核系统 tick)
 */
typedef uint32_t nos_timeout_t;

/**
 * 特殊超时常量
 * NOS_NO_WAIT: 非阻塞立即返回
 * NOS_WAIT_FOREVER: 无限期等待直到被唤醒
 */
#define NOS_NO_WAIT       ((nos_timeout_t)0U)
#define NOS_WAIT_FOREVER  ((nos_timeout_t)UINT32_MAX)

/**
 * 将相对超时转换为绝对单调截止时间戳 (ticks)
 *
 * @param timeout: 相对超时时间 (ticks)
 * @param out_deadline: 输出的单调绝对截止时间戳
 * @return: 0 表示成功，-EINVAL 参数无效，-EOVERFLOW 算术溢出
 */
int nos_deadline_from_timeout(nos_timeout_t timeout, uint64_t *out_deadline);

/**
 * 判断绝对截止时间是否已经到期
 *
 * @param deadline: 绝对截止时间戳 (UINT64_MAX 表示永不到期)
 * @return: true 表示已到期，false 表示未到期
 */
bool nos_deadline_is_expired(uint64_t deadline);

/**
 * 依据显式基准时间戳计算单调截止时间 (便于测试和确定性调度)
 *
 * @param timeout: 相对超时时间 (ticks)
 * @param now: 当前基准时间戳
 * @param out_deadline: 输出的单调绝对截止时间戳
 * @return: 0 表示成功，-EINVAL 参数无效，-EOVERFLOW 算术溢出
 */
static inline int nos_deadline_from_ticks(nos_timeout_t timeout, uint64_t now,
                                          uint64_t *out_deadline)
{
    if (out_deadline == NULL) {
        return -EINVAL;
    }

    if (timeout == NOS_WAIT_FOREVER) {
        *out_deadline = UINT64_MAX;
        return 0;
    }

    if (timeout > UINT64_MAX - now) {
        return -EOVERFLOW;
    }

    *out_deadline = now + (uint64_t)timeout;
    return 0;
}

#endif /* __NOS_TIMEOUT_H__ */
