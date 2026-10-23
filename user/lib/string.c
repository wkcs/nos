/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"

/* Freestanding string primitives and console output. */

void *u_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *u_memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;

    while (n--)
        *d++ = (unsigned char)value;
    return dst;
}

size_t u_strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

int u_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
#if !defined(NOS_NEWLIB) && !defined(NOS_MUSL)
void *memcpy(void *d, const void *s, size_t n)
{
    return u_memcpy(d, s, n);
}

void *memset(void *d, int value, size_t n)
{
    return u_memset(d, value, n);
}

size_t strlen(const char *s)
{
    return u_strlen(s);
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((uintptr_t)d > (uintptr_t)s && (uintptr_t)d - (uintptr_t)s < n) {
        while (n) {
            n--;
            d[n] = s[n];
        }
    } else {
        u_memcpy(dst, src, n);
    }
    return dst;
}
#if defined(NOS_ARM) || defined(NOS_ARM_M)
void __aeabi_memcpy(void *d, const void *s, size_t n)
{
    u_memcpy(d, s, n);
}

void __aeabi_memcpy4(void *d, const void *s, size_t n)
{
    u_memcpy(d, s, n);
}

void __aeabi_memclr(void *d, size_t n)
{
    u_memset(d, 0, n);
}

void __aeabi_memclr4(void *d, size_t n)
{
    u_memset(d, 0, n);
}
#endif
#endif
void nos_puts(const char *text)
{
    size_t remaining = u_strlen(text);
    while (remaining) {
        size_t count = remaining < 1024 ? remaining : 1024;
        intptr_t rc = nos_syscall(NOS_SYS_WRITE, (uintptr_t)text, count, 0);
        if (rc <= 0)
            break;
        text += rc;
        remaining -= (size_t)rc;
    }
}
