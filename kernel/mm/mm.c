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

__attribute__((weak)) void *mm_backend_alloc(size_t size, size_t align, gfp_t flag, pid_t pid)
{
    (void)align;
    return __kalloc((u32)size, flag, pid);
}

__attribute__((weak)) int mm_backend_free(void *addr)
{
    return __kfree(addr);
}

__attribute__((weak)) void *mm_backend_realloc(void *ptr, size_t size, gfp_t flag)
{
    (void)ptr;
    (void)size;
    (void)flag;
    return NULL;
}

__attribute__((weak)) int mm_backend_free_by_pid(pid_t pid)
{
    return __kfree_by_pid(pid);
}

void *kmalloc(u32 size, gfp_t flag)
{
    return mm_backend_alloc(size, sizeof(addr_t), flag, 0);
}

void *kzalloc(u32 size, gfp_t flag)
{
    void *buf;
    buf = mm_backend_alloc(size, sizeof(addr_t), flag, 0);
    if (buf == NULL)
        return NULL;
    memset(buf, 0, size);
    return buf;
}

void *kmalloc_aligned(size_t size, size_t align, gfp_t flag)
{
    return mm_backend_alloc(size, align, flag, 0);
}

void *kzalloc_aligned(size_t size, size_t align, gfp_t flag)
{
    void *buf = mm_backend_alloc(size, align, flag, 0);
    if (buf == NULL) {
        return NULL;
    }
    memset(buf, 0, size);
    return buf;
}

void *krealloc(void *ptr, u32 size, gfp_t flag)
{
    void *buf = NULL;
    struct memblock *block;
    struct mem_base *base;

    if (size == 0) {
        if (ptr != NULL) mm_backend_free(ptr);
        return NULL;
    }
    if (ptr == NULL)
        return mm_backend_alloc(size, sizeof(addr_t), flag, 0);

    buf = mm_backend_realloc(ptr, size, flag);
    if (buf != NULL) {
        return buf;
    }

    block = find_block(ptr);
    if (block == NULL) {
        pr_err("no memblock found for 0x%lx\n", (addr_t)ptr);
        return NULL;
    }
    base = find_base(block, ptr);
    if (base == NULL) {
        pr_err("no mem_base found for 0x%lx in memblock\n", (addr_t)ptr);
        return NULL;
    }

    buf = __kalloc(size, flag, 0);
    if (buf == NULL) {
        return NULL;
    }
    if (ptr != NULL)
        memcpy(buf, ptr, min(size, base->size));

    if (ptr != NULL)
        __kfree(ptr);
    return buf;
}

void *kalloc_by_pid(u32 size, gfp_t flag, pid_t pid)
{
    return mm_backend_alloc(size, sizeof(addr_t), flag, pid);
}

int kfree(void *addr)
{
    return mm_backend_free(addr);
}

int kfree_by_pid(pid_t pid)
{
    return mm_backend_free_by_pid(pid);
}
