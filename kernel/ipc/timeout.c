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
    return nos_deadline_from_ticks(timeout, cpu_run_ticks(), out_deadline);
}

bool nos_deadline_is_expired(uint64_t deadline)
{
    /* 无限期等待永不到期 */
    if (deadline == UINT64_MAX) {
        return false;
    }

    return cpu_run_ticks() >= deadline;
}
