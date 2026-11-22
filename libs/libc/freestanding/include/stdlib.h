/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 内核隔离式 freestanding <stdlib.h> 头文件（仅无工具链 libc 时启用）。
 */

#ifndef __NOS_FREESTANDING_STDLIB_H__
#define __NOS_FREESTANDING_STDLIB_H__

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void *calloc(size_t nitems, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

int atoi(const char *nptr);
long atol(const char *nptr);
long long atoll(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

int abs(int j);
long labs(long j);
long long llabs(long long j);

#endif /* __NOS_FREESTANDING_STDLIB_H__ */
