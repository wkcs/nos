/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/sleep.h>
#include <kernel/kernel.h>
#include <kernel/cpu.h>

void usleep(u32 usec)
{
    cpu_delay_us(usec);
}

void msleep(u32 msec)
{
    int i;

    for (i = msec; i > 0; i--) {
        usleep(1000);
    }
}

void sleep(u32 sec)
{
    int i;

    for (i = sec; i > 0; i--) {
        msleep(1000);
    }
}
