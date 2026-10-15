/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
__attribute__((weak)) intptr_t nos_syscall(uintptr_t number, uintptr_t a, uintptr_t b, uintptr_t c)
{
#if defined(NOS_ARM64)
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
    return (intptr_t)r0;
}
__attribute__((weak)) void nos_app_libc_init(int argc, char **argv) { (void)argc; (void)argv; }
__attribute__((weak, noreturn)) void nos_app_finish(int status)
{
    nos_syscall(NOS_SYS_EXIT, (uintptr_t)status, 0, 0);
    for (;;) { }
}
extern int main(int argc, char **argv);
void nos_app_start(uintptr_t argc, uintptr_t argv)
{
    nos_app_libc_init((int)argc, (char **)argv);
    int result = main((int)argc, (char **)argv);
    nos_app_finish(result);
}
