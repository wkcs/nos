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
#include <kernel/minmax.h>
#include <string.h>

extern void *__kalloc(u32 size, gfp_t flag, pid_t pid);
extern int __kfree(void *addr);
extern int __kfree_by_pid(pid_t pid);

void *kmalloc(u32 size, gfp_t flag)
{
    void *buf;
    buf = __kalloc(size, flag, 0);
    //pr_info("region:0x%08lx - 0x%08lx\r\n", (addr_t)buf, (addr_t)buf + size);
    return buf;
}

void *kzalloc(u32 size, gfp_t flag)
{
    void *buf;
    buf = __kalloc(size, flag, 0);
    if (buf == NULL)
        return NULL;
    memset(buf, 0, size);
    return buf;
}

void *krealloc(void *ptr, u32 size, gfp_t flag)
{
    void *buf = NULL;
    struct memblock *block;
    struct mem_base *base;

    if (size == 0)
        goto out;
    if (ptr == NULL)
        goto alloc;

    block = find_block(ptr);
    if (block == NULL) {
        pr_err("no memblock found for 0x%lx\r\n", (addr_t)ptr);
        goto out;
    }
    base = find_base(block, ptr);
    if (base == NULL) {
        pr_err("no mem_base found for 0x%lx in memblock\r\n", (addr_t)ptr);
        goto out;
    }

alloc:
    buf = __kalloc(size, flag, 0);
    if (buf == NULL) {
        goto out;
    }
    if (ptr != NULL)
        memcpy(buf, ptr, min(size, base->size));

out:
    if (ptr != NULL)
        __kfree(ptr);
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
