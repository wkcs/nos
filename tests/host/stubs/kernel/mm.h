/** Copyright (C) 2026 Nick Hu */
#ifndef NOS_TEST_MM_H
#define NOS_TEST_MM_H
#include <stddef.h>
#include <kernel/kernel.h>
void *kmalloc(size_t size, gfp_t flags);
void kfree(void *ptr);
#endif
