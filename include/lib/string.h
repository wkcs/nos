/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __STRING_H__
#define __STRING_H__

#include <kernel/kernel.h>

void *memcpy(void *dest, const void *src, size_t count);
size_t strlen(const char *s);
void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *s, int c, size_t count);
void *memmove(void *dest, const void *src, size_t count);
__visible int memcmp(const void *cs, const void *ct, size_t count);
void *memscan(void *addr, int c, size_t size);
char *strstr(const char *s1, const char *s2);
char *strnstr(const char *s1, const char *s2, size_t len);
void *memchr(const void *s, int c, size_t n);
void *memchr_inv(const void *start, int c, size_t bytes);
char *strreplace(char *s, char old, char new);
int strncmp(const char *cs, const char *ct, size_t count);
int strcmp(const char *cs, const char *ct);

#endif