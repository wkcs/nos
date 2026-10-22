/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"

/* Clock conversion, snapshots, and interruptible long sleeps. */

void nos_time_from_ticks(uint64_t ticks, struct nos_time_value *value)
{
    value->seconds = (int64_t)(ticks / 100);
    value->nanoseconds = (uint32_t)(ticks % 100) * 10000000u;
}

int nos_clock_snapshot(struct nos_time_value *value)
{
    uint64_t ticks;
    int rc = (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&ticks, 0, 0);
    if (!rc)
        nos_time_from_ticks(ticks, value);
    return rc;
}

int nos_clock_read(unsigned clock, struct nos_time_value *value)
{
    struct nos_clock_value snapshot;
    int rc = (int)nos_syscall(NOS_SYS_CLOCK_GET64, clock, (uintptr_t)&snapshot, 0);
    if (rc)
        return rc;
    if (snapshot.seconds < 0 || snapshot.nanoseconds < 0 || snapshot.nanoseconds >= 1000000000)
        return -NOS_EIO;
    struct nos_time_value result = { snapshot.seconds, (uint32_t)snapshot.nanoseconds };
    return nos_user_copy(value, &result, sizeof(result));
}

int nos_sleep_ticks64(uint64_t ticks, uint64_t *remaining)
{
    uint64_t start, now;
    int rc = (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&start, 0, 0);
    if (rc)
        return rc;
    if (ticks > UINT64_MAX - start)
        return -NOS_EOVERFLOW;
    now = start;
    while (now - start < ticks) {
        uint64_t left = ticks - (now - start);
        rc = (int)nos_syscall(NOS_SYS_SLEEP, left > 10000 ? 10000 : (uintptr_t)left, 0, 0);
        int clock_rc = (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&now, 0, 0);
        if (clock_rc)
            return clock_rc;
        if (rc) {
            if (remaining)
                *remaining = now - start >= ticks ? 0 : ticks - (now - start);
            return rc;
        }
    }
    if (remaining)
        *remaining = 0;
    return 0;
}
