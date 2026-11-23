/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS EL0 用户态 AI 原生系统调用 SDK（零 libc 依赖，兼容 ARM64 AArch64 SVC #0 ABI）
 */

#ifndef __NOS_USER_AI_H__
#define __NOS_USER_AI_H__

#include <uapi/nos/ai.h>
#include <uapi/nos/cap.h>
#include <uapi/nos/errno.h>
#include <uapi/nos/syscall.h>
#include <uapi/nos/types.h>
#include <nos/elf64.h>

#define NOS_USER_PROT_READ      (1U << 0)
#define NOS_USER_PROT_WRITE     (1U << 1)
#define NOS_USER_PROT_EXEC      (1U << 2)

#if defined(__aarch64__)

static inline long nos_user_syscall6(unsigned long sysno,
                                     unsigned long a0,
                                     unsigned long a1,
                                     unsigned long a2,
                                     unsigned long a3,
                                     unsigned long a4,
                                     unsigned long a5)
{
    register unsigned long x0 __asm__("x0") = a0;
    register unsigned long x1 __asm__("x1") = a1;
    register unsigned long x2 __asm__("x2") = a2;
    register unsigned long x3 __asm__("x3") = a3;
    register unsigned long x4 __asm__("x4") = a4;
    register unsigned long x5 __asm__("x5") = a5;
    register unsigned long x8 __asm__("x8") = sysno;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory");
    return (long)x0;
}

#else

static inline long nos_user_syscall6(unsigned long sysno,
                                     unsigned long a0,
                                     unsigned long a1,
                                     unsigned long a2,
                                     unsigned long a3,
                                     unsigned long a4,
                                     unsigned long a5)
{
    (void)sysno;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return -NOS_ENOSYS;
}

#endif

static inline long nos_user_syscall0(unsigned long sysno)
{
    return nos_user_syscall6(sysno, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL);
}

static inline long nos_user_syscall1(unsigned long sysno, unsigned long a0)
{
    return nos_user_syscall6(sysno, a0, 0UL, 0UL, 0UL, 0UL, 0UL);
}

static inline long nos_user_syscall2(unsigned long sysno, unsigned long a0, unsigned long a1)
{
    return nos_user_syscall6(sysno, a0, a1, 0UL, 0UL, 0UL, 0UL);
}

static inline long nos_user_syscall3(unsigned long sysno,
                                     unsigned long a0,
                                     unsigned long a1,
                                     unsigned long a2)
{
    return nos_user_syscall6(sysno, a0, a1, a2, 0UL, 0UL, 0UL);
}

static inline long nos_user_syscall4(unsigned long sysno,
                                     unsigned long a0,
                                     unsigned long a1,
                                     unsigned long a2,
                                     unsigned long a3)
{
    return nos_user_syscall6(sysno, a0, a1, a2, a3, 0UL, 0UL);
}

static inline void nos_user_yield(void)
{
    (void)nos_user_syscall0(SYS_NOS_YIELD);
}

static inline void nos_user_exit(int code)
{
    (void)nos_user_syscall1(SYS_NOS_EXIT, (unsigned long)(long)code);
    for (;;) {
    }
}

static inline long nos_user_console_write(const char *buf, size_t len)
{
    return nos_user_syscall3(SYS_NOS_DEBUG_CONSOLE,
                             (unsigned long)(uintptr_t)buf,
                             (unsigned long)len,
                             0UL);
}

static inline void nos_user_puts(const char *s)
{
    size_t len = 0U;

    if (s == NULL) {
        return;
    }
    while (s[len] != '\0') {
        len++;
    }
    if (len > 0U) {
        (void)nos_user_console_write(s, len);
    }
}

static inline long nos_user_clock_get(uint64_t *out_ns)
{
    return nos_user_syscall1(SYS_NOS_CLOCK_GET, (unsigned long)(uintptr_t)out_ns);
}

static inline int nos_user_vmo_create(uint64_t size_bytes,
                                      uint32_t kind,
                                      uint32_t flags,
                                      nos_handle_t *out_cap)
{
    struct nos_vmo_create_args args;
    nos_handle_t h = NOS_HANDLE_INVALID;
    long rc;

    args.size_bytes = size_bytes;
    args.kind = kind;
    args.flags = flags;
    args.block_bytes = 0U;
    args.reserved = 0U;
    args.reserved_ext = 0ULL;

    rc = nos_user_syscall2(SYS_NOS_VMO_CREATE,
                           (unsigned long)(uintptr_t)&args,
                           (unsigned long)(uintptr_t)&h);
    if (rc < 0) {
        return (int)rc;
    }
    if (out_cap != NULL) {
        *out_cap = h;
    }
    return 0;
}

static inline int nos_user_vmo_seal(nos_handle_t vmo_cap)
{
    return (int)nos_user_syscall2(SYS_NOS_VMO_CONTROL, NOS_VMO_OP_SEAL, (unsigned long)vmo_cap);
}

static inline int nos_user_vmo_map(nos_handle_t vmo_cap,
                                   nos_handle_t target_cell_cap,
                                   uintptr_t vaddr,
                                   size_t size_bytes,
                                   uint32_t prot_flags)
{
    return (int)nos_user_syscall6(SYS_NOS_VMO_CONTROL,
                                  NOS_VMO_OP_MAP,
                                  (unsigned long)vmo_cap,
                                  (unsigned long)target_cell_cap,
                                  (unsigned long)vaddr,
                                  (unsigned long)size_bytes,
                                  (unsigned long)prot_flags);
}

static inline int nos_user_vmo_unmap(nos_handle_t vmo_cap,
                                     nos_handle_t target_cell_cap,
                                     uintptr_t vaddr,
                                     size_t size_bytes)
{
    return (int)nos_user_syscall6(SYS_NOS_VMO_CONTROL,
                                  NOS_VMO_OP_UNMAP,
                                  (unsigned long)vmo_cap,
                                  (unsigned long)target_cell_cap,
                                  (unsigned long)vaddr,
                                  (unsigned long)size_bytes,
                                  0UL);
}

static inline int nos_user_ring_create(const struct nos_ring_create_args *args,
                                       nos_handle_t *out_cap)
{
    nos_handle_t h = NOS_HANDLE_INVALID;
    long rc = nos_user_syscall2(SYS_NOS_RING_CREATE,
                                (unsigned long)(uintptr_t)args,
                                (unsigned long)(uintptr_t)&h);
    if (rc < 0) {
        return (int)rc;
    }
    if (out_cap != NULL) {
        *out_cap = h;
    }
    return 0;
}

static inline int nos_user_ring_enter(nos_handle_t ring_cap,
                                      uint32_t to_submit,
                                      uint32_t min_complete,
                                      uint32_t flags,
                                      uint32_t timeout_ticks)
{
    return (int)nos_user_syscall6(SYS_NOS_RING_ENTER,
                                  (unsigned long)ring_cap,
                                  (unsigned long)to_submit,
                                  (unsigned long)min_complete,
                                  (unsigned long)flags,
                                  (unsigned long)timeout_ticks,
                                  0UL);
}

static inline int nos_user_sched_query_topo(struct nos_cpu_topo_info *out_topo)
{
    return (int)nos_user_syscall3(SYS_NOS_SCHED_INFER,
                                  NOS_SCHED_OP_QUERY_TOPO,
                                  0UL,
                                  (unsigned long)(uintptr_t)out_topo);
}

static inline int nos_user_accel_query_caps(nos_handle_t accel_cap,
                                            struct nos_accel_caps *out_caps)
{
    return (int)nos_user_syscall4(SYS_NOS_ACCEL_SUBMIT,
                                  NOS_ACCEL_OP_QUERY_CAPS,
                                  (unsigned long)accel_cap,
                                  (unsigned long)(uintptr_t)out_caps,
                                  0UL);
}

static inline int nos_user_accel_submit(nos_handle_t accel_cap,
                                        const struct nos_accel_job_desc *desc,
                                        struct nos_accel_job_result *out_res)
{
    return (int)nos_user_syscall4(SYS_NOS_ACCEL_SUBMIT,
                                  NOS_ACCEL_OP_SUBMIT,
                                  (unsigned long)accel_cap,
                                  (unsigned long)(uintptr_t)desc,
                                  (unsigned long)(uintptr_t)out_res);
}

static inline int nos_user_cap_revoke_tree(nos_handle_t handle, uint32_t *out_revoked)
{
    return (int)nos_user_syscall2(SYS_NOS_CAP_REVOKE_TREE,
                                  (unsigned long)handle,
                                  (unsigned long)(uintptr_t)out_revoked);
}

static inline void nos_user_memset(void *dst, uint8_t val, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0U; i < n; i++) {
        p[i] = val;
    }
}

#endif /* __NOS_USER_AI_H__ */
