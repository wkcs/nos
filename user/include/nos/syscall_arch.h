/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Multi-Architecture User-Space / SASOS System Call Invocation Primitives
 */

#ifndef __NOS_USER_SYSCALL_ARCH_H__
#define __NOS_USER_SYSCALL_ARCH_H__

#include <uapi/nos/types.h>
#include <uapi/nos/syscall.h>

#if !defined(CONFIG_USER_TRAP_SYSCALL)

/*
 * Single-Address-Space OS (SASOS) / MCU Fastpath:
 * Invokes the microkernel syscall dispatcher directly without trap overhead
 * while preserving the exact 6-register microkernel syscall ABI.
 */
extern long nos_syscall_dispatch(unsigned long sysno,
                                 uintptr_t a0,
                                 uintptr_t a1,
                                 uintptr_t a2,
                                 uintptr_t a3,
                                 uintptr_t a4,
                                 uintptr_t a5);

static inline long __nos_syscall6(unsigned long sysno,
                                  uintptr_t a0,
                                  uintptr_t a1,
                                  uintptr_t a2,
                                  uintptr_t a3,
                                  uintptr_t a4,
                                  uintptr_t a5)
{
    return nos_syscall_dispatch(sysno, a0, a1, a2, a3, a4, a5);
}

#elif defined(__aarch64__)

static inline long __nos_syscall6(unsigned long sysno,
                                  uintptr_t a0,
                                  uintptr_t a1,
                                  uintptr_t a2,
                                  uintptr_t a3,
                                  uintptr_t a4,
                                  uintptr_t a5)
{
    register uintptr_t x0 __asm__("x0") = a0;
    register uintptr_t x1 __asm__("x1") = a1;
    register uintptr_t x2 __asm__("x2") = a2;
    register uintptr_t x3 __asm__("x3") = a3;
    register uintptr_t x4 __asm__("x4") = a4;
    register uintptr_t x5 __asm__("x5") = a5;
    register uintptr_t x8 __asm__("x8") = sysno;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory", "cc");
    return (long)x0;
}

#elif defined(__riscv)

static inline long __nos_syscall6(unsigned long sysno,
                                  uintptr_t a0,
                                  uintptr_t a1,
                                  uintptr_t a2,
                                  uintptr_t a3,
                                  uintptr_t a4,
                                  uintptr_t a5)
{
    register uintptr_t r0 __asm__("a0") = a0;
    register uintptr_t r1 __asm__("a1") = a1;
    register uintptr_t r2 __asm__("a2") = a2;
    register uintptr_t r3 __asm__("a3") = a3;
    register uintptr_t r4 __asm__("a4") = a4;
    register uintptr_t r5 __asm__("a5") = a5;
    register uintptr_t r7 __asm__("a7") = sysno;
    __asm__ volatile("ecall"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
                     : "memory");
    return (long)r0;
}

#elif defined(__x86_64__)

static inline long __nos_syscall6(unsigned long sysno,
                                  uintptr_t a0,
                                  uintptr_t a1,
                                  uintptr_t a2,
                                  uintptr_t a3,
                                  uintptr_t a4,
                                  uintptr_t a5)
{
    long ret;
    register uintptr_t r10 __asm__("r10") = a3;
    register uintptr_t r8 __asm__("r8") = a4;
    register uintptr_t r9 __asm__("r9") = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(sysno), "D"(a0), "S"(a1), "d"(a2),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

#elif defined(__arm__)

static inline long __nos_syscall6(unsigned long sysno,
                                  uintptr_t a0,
                                  uintptr_t a1,
                                  uintptr_t a2,
                                  uintptr_t a3,
                                  uintptr_t a4,
                                  uintptr_t a5)
{
    register uintptr_t r0 __asm__("r0") = a0;
    register uintptr_t r1 __asm__("r1") = a1;
    register uintptr_t r2 __asm__("r2") = a2;
    register uintptr_t r3 __asm__("r3") = a3;
    register uintptr_t r4 __asm__("r4") = a4;
    register uintptr_t r5 __asm__("r5") = a5;
    register uintptr_t r7 __asm__("r7") = sysno;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
                     : "memory", "cc");
    return (long)r0;
}

#endif

static inline long nos_syscall0(unsigned long n)
{
    return __nos_syscall6(n, 0, 0, 0, 0, 0, 0);
}

static inline long nos_syscall1(unsigned long n, uintptr_t a0)
{
    return __nos_syscall6(n, a0, 0, 0, 0, 0, 0);
}

static inline long nos_syscall2(unsigned long n, uintptr_t a0, uintptr_t a1)
{
    return __nos_syscall6(n, a0, a1, 0, 0, 0, 0);
}

static inline long nos_syscall3(unsigned long n, uintptr_t a0, uintptr_t a1, uintptr_t a2)
{
    return __nos_syscall6(n, a0, a1, a2, 0, 0, 0);
}

static inline long nos_syscall4(unsigned long n, uintptr_t a0, uintptr_t a1,
                                uintptr_t a2, uintptr_t a3)
{
    return __nos_syscall6(n, a0, a1, a2, a3, 0, 0);
}

static inline long nos_syscall5(unsigned long n, uintptr_t a0, uintptr_t a1,
                                uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    return __nos_syscall6(n, a0, a1, a2, a3, a4, 0);
}

static inline long nos_syscall6(unsigned long n, uintptr_t a0, uintptr_t a1,
                                uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    return __nos_syscall6(n, a0, a1, a2, a3, a4, a5);
}

#endif /* __NOS_USER_SYSCALL_ARCH_H__ */
