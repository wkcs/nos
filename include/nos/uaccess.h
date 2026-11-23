/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 可移植安全用户态-内核态内存拷贝接口（配合 ARM64 PAN 与 __ex_table 故障修复）
 */

#ifndef __NOS_UACCESS_H__
#define __NOS_UACCESS_H__

#include <stddef.h>
#include <stdint.h>

#ifndef __HOST_TEST__
#include <autocfg.h>
#endif

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)

int nos_copy_from_user(void *kdst, uintptr_t usrc, size_t n);
int nos_copy_to_user(uintptr_t udst, const void *ksrc, size_t n);
long nos_strncpy_from_user(char *kdst, uintptr_t usrc, size_t max_len);

#else

#include <uapi/nos/errno.h>
#include <string.h>

static inline int nos_copy_from_user(void *kdst, uintptr_t usrc, size_t n)
{
    if (n == 0U) {
        return 0;
    }
    if (kdst == NULL || usrc == 0U) {
        return -EFAULT;
    }
    memcpy(kdst, (const void *)usrc, n);
    return 0;
}

static inline int nos_copy_to_user(uintptr_t udst, const void *ksrc, size_t n)
{
    if (n == 0U) {
        return 0;
    }
    if (udst == 0U || ksrc == NULL) {
        return -EFAULT;
    }
    memcpy((void *)udst, ksrc, n);
    return 0;
}

static inline long nos_strncpy_from_user(char *kdst, uintptr_t usrc, size_t max_len)
{
    const char *src;
    size_t i = 0U;

    if (kdst == NULL || usrc == 0U || max_len == 0U) {
        return -EFAULT;
    }
    src = (const char *)usrc;
    while (i + 1U < max_len && src[i] != '\0') {
        kdst[i] = src[i];
        i++;
    }
    kdst[i] = '\0';
    return (long)i;
}

#endif

#endif /* __NOS_UACCESS_H__ */
