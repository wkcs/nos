/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 内核隔离式 freestanding <string.h> 头文件（仅无工具链 libc 时启用）。
 */

#ifndef __NOS_FREESTANDING_STRING_H__
#define __NOS_FREESTANDING_STRING_H__

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
void *memset(void *s, int c, size_t count);
int memcmp(const void *cs, const void *ct, size_t count);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t count);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t count);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t count);
int strcmp(const char *cs, const char *ct);
int strncmp(const char *cs, const char *ct, size_t count);
int strcoll(const char *a, const char *b);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *s1, const char *s2);
char *strpbrk(const char *cs, const char *ct);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strdup(const char *s);
char *strerror(int errnum);

#endif /* __NOS_FREESTANDING_STRING_H__ */
