/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
typedef int gfp_t;
struct mm_buddy_info { struct list_head list; spinlock_t lock; u32 free_num, order; };
struct mm_buddy { struct mm_buddy_info info[CONFIG_MAX_ORDER]; };
struct mm_node { const char *name; addr_t start; uintptr_t start_pfn; u32 page_num, free_num, buddy_page_index; struct mm_buddy buddy; };
struct page { struct mm_node *node; u32 use_cnt; int private; uintptr_t pfn; struct list_head list; };
struct mm_node *find_mm_node(addr_t addr);
addr_t __alloc_pages_exact(struct mm_buddy *buddy, size_t size, gfp_t flag);
int __free_pages_exact(addr_t addr, size_t size);
