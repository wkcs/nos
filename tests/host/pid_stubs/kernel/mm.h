/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <stdlib.h>
static inline void *kmalloc(size_t n, int flags) { (void)flags; return malloc(n); }
static inline void kfree(void *p) { free(p); }
