#pragma once
#include <stdlib.h>
#include <string.h>
extern int ramfs_fail_alloc;
static inline void *kzalloc(size_t n, int flags) { (void)flags; return calloc(1, n); }
static inline void *kmalloc(size_t n, int flags) { (void)flags; return ramfs_fail_alloc ? NULL : malloc(n); }
static inline void *krealloc(void *p, size_t n, int flags) {
    (void)flags;
    if (ramfs_fail_alloc) { free(p); return NULL; }
    return realloc(p, n);
}
static inline void kfree(void *p) { free(p); }
