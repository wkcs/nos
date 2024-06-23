/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __STRING_H__
#define __STRING_H__

#include <kernel/kernel.h>
#include <kernel/mm.h>

char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t count);
size_t strlcat(char *dest, const char *src, size_t count);
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
char * strchr(const char *,int);
char *strpbrk(const char *cs, const char *ct);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t count);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
size_t strnlen(const char *s, size_t count);
int strcoll (const char *a, const char *b);
char *kstrdup(const char *s, gfp_t gfp);
char *strerror (int errnum);

#endif