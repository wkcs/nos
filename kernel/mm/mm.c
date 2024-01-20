/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[MM]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/mm.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/pid.h>
#include <kernel/task.h>

extern void *__kalloc(u32 size, gfp_t flag, pid_t pid);
extern int __kfree(void *addr);
extern int __kfree_by_pid(pid_t pid);

void *kalloc(u32 size, gfp_t flag)
{
    void *buf;
    buf = __kalloc(size, flag, 0);
    //pr_info("region:0x%08lx - 0x%08lx\r\n", (addr_t)buf, (addr_t)buf + size);
    return buf;
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
