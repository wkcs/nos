/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
typedef int gfp_t;
struct mem_base { u32 magic; pid_t pid; u32 size; u32 used; struct list_head list; };
struct memblock { addr_t start; size_t size; u32 max_alloc_cap; struct list_head base; spinlock_t lock; struct list_head list; };
addr_t alloc_pages(gfp_t flag, u32 order);
int free_pages(addr_t addr, u32 order);
struct memblock *find_block(void *addr);
struct mem_base *find_base(struct memblock *block, void *addr);
