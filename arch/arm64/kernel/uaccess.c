/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 安全用户空间访存包装实现（边界拦截 + 硬件 PAN 隔离 + __ex_table 故障恢复）
 */

#include <nos/uaccess.h>
#include <uapi/nos/errno.h>
#include <kernel/task.h>
#include <asm/cpufeature.h>
#include <asm/mmu.h>
#include <string.h>

extern int __arm64_raw_copy_from_user(void *kdst, uintptr_t usrc, size_t n, uint32_t has_pan);
extern int __arm64_raw_copy_to_user(uintptr_t udst, const void *ksrc, size_t n, uint32_t has_pan);

static bool uaccess_range_is_user(uintptr_t uaddr, size_t n)
{
    uintptr_t end;

    if (uaddr < (uintptr_t)PAGE_SIZE || uaddr >= (uintptr_t)ARM64_USER_VA_LIMIT) {
        return false;
    }
    end = uaddr + n;
    if (end < uaddr || end > (uintptr_t)ARM64_USER_VA_LIMIT) {
        return false;
    }
    return true;
}

static bool uaccess_range_is_kernel(uintptr_t kaddr, size_t n)
{
    uintptr_t end = kaddr + n;

    if (kaddr < (uintptr_t)ARM64_PHYSMAP_BASE || end < kaddr) {
        return false;
    }
    return true;
}

static uint32_t uaccess_has_pan(void)
{
    const struct arm64_cpu_caps *caps = arm64_get_cpu_caps();
    return (caps != NULL && caps->has_pan) ? 1U : 0U;
}

int nos_copy_from_user(void *kdst, uintptr_t usrc, size_t n)
{
    struct task_struct *t = current;
    bool el0_only = (t != NULL && t->arch_in_el0_syscall != 0U);

    if (n == 0U) {
        return 0;
    }
    if (kdst == NULL || usrc == 0U) {
        return -EFAULT;
    }

    if (uaccess_range_is_user(usrc, n)) {
        return __arm64_raw_copy_from_user(kdst, usrc, n, uaccess_has_pan());
    }
    if (!el0_only && uaccess_range_is_kernel(usrc, n)) {
        memcpy(kdst, (const void *)usrc, n);
        return 0;
    }
    return -EFAULT;
}

int nos_copy_to_user(uintptr_t udst, const void *ksrc, size_t n)
{
    struct task_struct *t = current;
    bool el0_only = (t != NULL && t->arch_in_el0_syscall != 0U);

    if (n == 0U) {
        return 0;
    }
    if (udst == 0U || ksrc == NULL) {
        return -EFAULT;
    }

    if (uaccess_range_is_user(udst, n)) {
        return __arm64_raw_copy_to_user(udst, ksrc, n, uaccess_has_pan());
    }
    if (!el0_only && uaccess_range_is_kernel(udst, n)) {
        memcpy((void *)udst, ksrc, n);
        return 0;
    }
    return -EFAULT;
}

long nos_strncpy_from_user(char *kdst, uintptr_t usrc, size_t max_len)
{
    size_t i = 0U;

    if (kdst == NULL || max_len == 0U) {
        return -EFAULT;
    }
    kdst[0] = '\0';

    while (i + 1U < max_len) {
        char ch = '\0';
        int rc = nos_copy_from_user(&ch, usrc + i, 1U);
        if (rc < 0) {
            kdst[0] = '\0';
            return (long)rc;
        }
        kdst[i] = ch;
        if (ch == '\0') {
            return (long)i;
        }
        i++;
    }

    kdst[i] = '\0';
    return (long)i;
}
