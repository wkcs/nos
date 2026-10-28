#ifndef TEST_VFS_FD_MM_H
#define TEST_VFS_FD_MM_H
#include <stdlib.h>
static inline void kfree(void *p) { free(p); }
#endif
