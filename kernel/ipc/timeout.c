/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 通用超时机制与单调截止时间转换实现。
 */

#include <nos/timeout.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>

int nos_deadline_from_timeout(nos_timeout_t timeout, uint64_t *out_deadline)
{
    uint64_t now;

    if (out_deadline == NULL) {
        return -EINVAL;
    }

    /* NOS_WAIT_FOREVER 转换为最大绝对截止时间，永不到期 */
    if (timeout == NOS_WAIT_FOREVER) {
        *out_deadline = UINT64_MAX;
        return 0;
    }

    now = cpu_run_ticks();

    /* 检查算术溢出 */
    if (timeout > UINT64_MAX - now) {
        return -EOVERFLOW;
    }

    *out_deadline = now + (uint64_t)timeout;
    return 0;
}

bool nos_deadline_is_expired(uint64_t deadline)
{
    /* 无限期等待永不到期 */
    if (deadline == UINT64_MAX) {
        return false;
    }

    return cpu_run_ticks() >= deadline;
}
