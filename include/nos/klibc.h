/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Kernel internal C utility declarations.
 * Extends standard <string.h> without hijacking global standard headers.
 */

#ifndef __NOS_KLIBC_H__
#define __NOS_KLIBC_H__

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <kernel/types.h>

size_t strlcat(char *dest, const char *src, size_t count);
size_t strnlen(const char *s, size_t count);
void *memscan(void *addr, int c, size_t size);
char *strnstr(const char *s1, const char *s2, size_t len);
void *memchr_inv(const void *start, int c, size_t bytes);
char *strreplace(char *s, char old, char new);
char *kstrdup(const char *s, gfp_t gfp);

#endif /* __NOS_KLIBC_H__ */
