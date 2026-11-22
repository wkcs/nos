/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 内核隔离式 freestanding 运行时（供不自带 libc 的交叉工具链使用）。
 * 提供最小堆桥接、字符串扩展、整型解析、堆排序及控制台输出桥接。
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

#include <kernel/kernel.h>
#include <kernel/console.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <lib/vsprintf.h>
#include <fs/fs.h>
#include <fs/vfs.h>

struct __nos_freestanding_file {
    int fd;
};

static struct __nos_freestanding_file g_stdin_file  = { .fd = 0 };
static struct __nos_freestanding_file g_stdout_file = { .fd = 1 };
static struct __nos_freestanding_file g_stderr_file = { .fd = 2 };

FILE *stdin  = &g_stdin_file;
FILE *stdout = &g_stdout_file;
FILE *stderr = &g_stderr_file;

static int g_fallback_errno;

int *__nos_errno_location(void)
{
    /* 若任务结构体或每 CPU errno 就绪可在此扩展；当前返回共享静态槽 */
    return &g_fallback_errno;
}

void *malloc(size_t size)
{
    if (size == 0U) {
        size = 1U;
    }
    return kmalloc((u32)size, GFP_KERNEL);
}

void *calloc(size_t nitems, size_t size)
{
    if (nitems != 0U && size > (SIZE_MAX / nitems)) {
        errno = ENOMEM;
        return NULL;
    }
    return kzalloc((u32)(nitems * size), GFP_KERNEL);
}

void *realloc(void *ptr, size_t size)
{
    return krealloc(ptr, (u32)size, GFP_KERNEL);
}

void free(void *ptr)
{
    if (ptr != NULL) {
        kfree(ptr);
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    char ch = (char)c;

    if (s == NULL) {
        return NULL;
    }
    do {
        if (*s == ch) {
            last = s;
        }
    } while (*s++ != '\0');
    return (char *)last;
}

char *strdup(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s) + 1U;
    copy = (char *)malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

int abs(int j)
{
    return (j < 0) ? -j : j;
}

long labs(long j)
{
    return (j < 0L) ? -j : j;
}

long long llabs(long long j)
{
    return (j < 0LL) ? -j : j;
}

static unsigned long long parse_unsigned_impl(const char *nptr,
                                              char **endptr,
                                              int base,
                                              bool *out_neg,
                                              bool *out_any,
                                              bool *out_ovf,
                                              unsigned long long ulimit)
{
    const char *s = nptr;
    unsigned long long acc = 0ULL;
    unsigned long long cutoff;
    unsigned int cutlim;
    bool neg = false;
    bool any = false;
    bool ovf = false;

    if (nptr == NULL || (base != 0 && (base < 2 || base > 36))) {
        if (endptr != NULL) {
            *endptr = (char *)nptr;
        }
        errno = EINVAL;
        *out_neg = false;
        *out_any = false;
        *out_ovf = false;
        return 0ULL;
    }

    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '-') {
        neg = true;
        s++;
    } else if (*s == '+') {
        s++;
    }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (*s == '0') ? 8 : 10;
    }

    cutoff = ulimit / (unsigned long long)base;
    cutlim = (unsigned int)(ulimit % (unsigned long long)base);

    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        unsigned int digit;

        if (c >= '0' && c <= '9') {
            digit = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'z') {
            digit = (unsigned int)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'Z') {
            digit = (unsigned int)(c - 'A' + 10);
        } else {
            break;
        }
        if (digit >= (unsigned int)base) {
            break;
        }
        any = true;
        if (ovf || acc > cutoff || (acc == cutoff && digit > cutlim)) {
            ovf = true;
        } else {
            acc = acc * (unsigned long long)base + digit;
        }
    }

    if (endptr != NULL) {
        *endptr = (char *)(any ? s : nptr);
    }
    *out_neg = neg;
    *out_any = any;
    *out_ovf = ovf;
    return acc;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    bool neg, any, ovf;
    unsigned long long val = parse_unsigned_impl(nptr, endptr, base,
                                                 &neg, &any, &ovf, ULLONG_MAX);
    if (!any) {
        return 0ULL;
    }
    if (ovf) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return neg ? (0ULL - val) : val;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    bool neg, any, ovf;
    unsigned long long val = parse_unsigned_impl(nptr, endptr, base,
                                                 &neg, &any, &ovf, ULONG_MAX);
    if (!any) {
        return 0UL;
    }
    if (ovf) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return (unsigned long)(neg ? (0UL - (unsigned long)val) : (unsigned long)val);
}

long long strtoll(const char *nptr, char **endptr, int base)
{
    bool neg, any, ovf;
    unsigned long long limit = (unsigned long long)LLONG_MAX + 1ULL;
    unsigned long long val;

    /* 判断符号以分别确定正负边界 */
    const char *p = nptr;
    bool is_neg = false;
    if (p != NULL) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        is_neg = (*p == '-');
    }
    limit = is_neg ? ((unsigned long long)LLONG_MAX + 1ULL) : (unsigned long long)LLONG_MAX;
    val = parse_unsigned_impl(nptr, endptr, base, &neg, &any, &ovf, limit);
    if (!any) {
        return 0LL;
    }
    if (ovf) {
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    if (neg) {
        return (val == (unsigned long long)LLONG_MAX + 1ULL) ? LLONG_MIN : -(long long)val;
    }
    return (long long)val;
}

long strtol(const char *nptr, char **endptr, int base)
{
    bool neg, any, ovf;
    const char *p = nptr;
    bool is_neg = false;
    unsigned long long limit;
    unsigned long long val;

    if (p != NULL) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        is_neg = (*p == '-');
    }
    limit = is_neg ? ((unsigned long long)LONG_MAX + 1ULL) : (unsigned long long)LONG_MAX;
    val = parse_unsigned_impl(nptr, endptr, base, &neg, &any, &ovf, limit);
    if (!any) {
        return 0L;
    }
    if (ovf) {
        errno = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    if (neg) {
        return (val == (unsigned long long)LONG_MAX + 1ULL) ? LONG_MIN : -(long)val;
    }
    return (long)val;
}

int atoi(const char *nptr)
{
    return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr)
{
    return strtol(nptr, NULL, 10);
}

long long atoll(const char *nptr)
{
    return strtoll(nptr, NULL, 10);
}

static void swap_bytes(uint8_t *a, uint8_t *b, size_t size)
{
    while (size-- > 0U) {
        uint8_t tmp = *a;
        *a++ = *b;
        *b++ = tmp;
    }
}

static void sift_down(uint8_t *arr, size_t start, size_t count, size_t size,
                      int (*compar)(const void *, const void *))
{
    size_t root = start;

    while (root * 2U + 1U < count) {
        size_t child = root * 2U + 1U;
        if (child + 1U < count &&
            compar(arr + child * size, arr + (child + 1U) * size) < 0) {
            child++;
        }
        if (compar(arr + root * size, arr + child * size) >= 0) {
            break;
        }
        swap_bytes(arr + root * size, arr + child * size, size);
        root = child;
    }
}

/* 使用 O(1) 栈空间的原地堆排序实现 qsort，避免内核栈溢出风险 */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    uint8_t *arr = (uint8_t *)base;

    if (base == NULL || compar == NULL || nmemb < 2U || size == 0U) {
        return;
    }

    for (size_t start = (nmemb - 2U) / 2U + 1U; start > 0U; start--) {
        sift_down(arr, start - 1U, nmemb, size, compar);
    }
    for (size_t end = nmemb - 1U; end > 0U; end--) {
        swap_bytes(arr, arr + end * size, size);
        sift_down(arr, 0U, end, size, compar);
    }
}

int vprintf(const char *fmt, va_list args)
{
    char buf[256];
    int len;

    if (fmt == NULL) {
        return -1;
    }
    len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        int out_len = (len < (int)sizeof(buf)) ? len : (int)(sizeof(buf) - 1U);
        console_write(buf, (size_t)out_len);
    }
    return len;
}

int printf(const char *fmt, ...)
{
    va_list args;
    int len;

    va_start(args, fmt);
    len = vprintf(fmt, args);
    va_end(args);
    return len;
}

int putchar(int c)
{
    unsigned char ch = (unsigned char)c;
    console_write((const char *)&ch, 1U);
    return (int)ch;
}

int puts(const char *s)
{
    if (s == NULL) {
        return EOF;
    }
    console_write(s, strlen(s));
    console_write("\n", 1U);
    return 0;
}

int fputs(const char *s, FILE *stream)
{
    (void)stream;
    if (s == NULL) {
        return EOF;
    }
    console_write(s, strlen(s));
    return 0;
}

int fputc(int c, FILE *stream)
{
    (void)stream;
    return putchar(c);
}

int getchar(void)
{
    return console_getc();
}

int isatty(int fd)
{
    if (fd < 0) {
        errno = EBADF;
        return 0;
    }
#if defined(CONFIG_VFS) || defined(CONFIG_FS)
    struct file *file = vfs_fd_get(fd);
    if (file != NULL) {
        int is_tty = (file->f_inode != NULL && S_ISCHR(file->f_inode->i_mode)) ? 1 : 0;
        vfs_fd_put(file);
        return is_tty;
    }
#endif
    return (fd < 3) ? 1 : 0;
}
