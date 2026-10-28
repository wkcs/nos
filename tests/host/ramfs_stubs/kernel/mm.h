#pragma once
#include <stdlib.h>
#include <string.h>
static inline void *kzalloc(size_t n, int flags) { (void)flags; return calloc(1, n); }
static inline void *kmalloc(size_t n, int flags) { (void)flags; return malloc(n); }
static inline void *krealloc(void *p, size_t n, int flags) { (void)flags; return realloc(p, n); }
static inline void kfree(void *p) { free(p); }
