/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"

/* Architecture syscall entry and checked user-memory access. */

intptr_t nos_syscall(uintptr_t number, uintptr_t a, uintptr_t b, uintptr_t c)
{
#if defined(NOS_XTENSA)
    extern intptr_t nos_xtensa_syscall(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    return nos_xtensa_syscall(number, a, b, c);
#elif defined(NOS_ARM64)
    register uintptr_t r0 __asm__("x0") = a, r1 __asm__("x1") = b, r2 __asm__("x2") = c;
    register uintptr_t nr __asm__("x8") = number;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(nr) : "memory", "cc");
#elif defined(NOS_RISCV)
    register uintptr_t r0 __asm__("a0") = a, r1 __asm__("a1") = b, r2 __asm__("a2") = c;
    register uintptr_t nr __asm__("a7") = number;
    __asm__ volatile("ecall" : "+r"(r0) : "r"(r1), "r"(r2), "r"(nr) : "memory");
#else
    register uintptr_t r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c;
#ifdef NOS_ARM_M
    register uintptr_t nr __asm__("r12") = number;
#else
    register uintptr_t nr __asm__("r7") = number;
#endif
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(nr) : "memory", "cc");
#endif
#ifndef NOS_XTENSA
    return (intptr_t)r0;
#endif
}

int nos_user_check(const void *pointer, size_t size, unsigned protection)
{
    if (protection == (NOS_PROT_READ | NOS_PROT_WRITE)) {
        int rc = (int)nos_syscall(NOS_SYS_USER_CHECK, (uintptr_t)pointer, size, NOS_PROT_READ);
        return rc ? rc : (int)nos_syscall(NOS_SYS_USER_CHECK, (uintptr_t)pointer, size, NOS_PROT_WRITE);
    }
    return (int)nos_syscall(NOS_SYS_USER_CHECK, (uintptr_t)pointer, size, protection);
}

int nos_user_copy(void *destination, const void *source, size_t size)
{
    return (int)nos_syscall(NOS_SYS_USER_COPY, (uintptr_t)destination, (uintptr_t)source, size);
}
