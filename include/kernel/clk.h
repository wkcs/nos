/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_CLK_H__
#define __NOS_CLK_H__

#include <kernel/kernel.h>
#include <kernel/cpu.h>

inline uint32_t tick_to_msec(uint32_t tick)
{
    return tick * CONFIG_SYS_TICK_MS;
}

inline uint64_t tick_to_msec_64(uint64_t tick)
{
    return tick * CONFIG_SYS_TICK_MS;
}

inline uint32_t tick_to_sec(uint32_t tick)
{
    return tick * CONFIG_SYS_TICK_MS / 1000;
}

inline uint64_t tick_to_sec_64(uint64_t tick)
{
    return tick * CONFIG_SYS_TICK_MS / 1000;
}

inline uint32_t msec_to_tick(uint32_t msec)
{
    if (msec < CONFIG_SYS_TICK_MS) {
        pr_warning("msec min is %d\r\n", CONFIG_SYS_TICK_MS);
        return 1;
    }

    return msec / CONFIG_SYS_TICK_MS;
}

inline uint64_t msec_to_tick_64(uint64_t msec)
{
    if (msec < CONFIG_SYS_TICK_MS) {
        pr_warning("msec min is %d\r\n", CONFIG_SYS_TICK_MS);
        return 1;
    }

    return msec / CONFIG_SYS_TICK_MS;
}

inline uint32_t sec_to_tick(uint32_t sec)
{
    return sec * 1000 / CONFIG_SYS_TICK_MS;
}

inline uint64_t sec_to_tick_64(uint64_t sec)
{
    return sec * 1000 / CONFIG_SYS_TICK_MS;
}

#endif /* __NOS_CLK_H__ */
