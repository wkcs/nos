#ifndef NOS_TEST_W25Q_MM_H
#define NOS_TEST_W25Q_MM_H
#include <stddef.h>
#define GFP_KERNEL 0
void *kmalloc(size_t size, unsigned int flags);
void kfree(void *ptr);
#endif
