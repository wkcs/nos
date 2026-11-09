#ifndef TEST_MM_H
#define TEST_MM_H
#include <stdlib.h>
#include <kernel/kernel.h>
static inline void *kmalloc(u32 size, int flags) { (void)flags; return malloc(size); }
static inline void *kzalloc(u32 size, int flags) { (void)flags; return calloc(1, size); }
static inline void *krealloc(void *ptr, u32 size, int flags) { (void)flags; return realloc(ptr, size); }
static inline void kfree(void *p) { free(p); }
#endif
