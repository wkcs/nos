/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/mm.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/pid.h>

extern void *__kalloc(u32 size, gfp_t flag, pid_t pid);
extern int __kfree(void *addr);
extern int __kfree_by_pid(pid_t pid);

void *kalloc(u32 size, gfp_t flag)
{
    return __kalloc(size, flag, 0);
}

void *kalloc_by_pid(u32 size, gfp_t flag, pid_t pid)
{
    return __kalloc(size, flag, pid);
}

int kfree(void *addr)
{
    return __kfree(addr);
}

int kfree_by_pid(pid_t pid)
{
    return __kfree_by_pid(pid);
}
