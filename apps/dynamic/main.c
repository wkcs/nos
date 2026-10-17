/* Copyright (C) 2026 NOS contributors */
#include <stdint.h>
#include <stddef.h>
extern int nos_dynamic_add(int value);
extern int nos_dynamic_initialized;
static long raw_call(long number, uintptr_t first, uintptr_t second, uintptr_t third)
{
#if defined(__aarch64__)
    register uintptr_t a __asm__("x0") = first, b __asm__("x1") = second, c __asm__("x2") = third;
    register long nr __asm__("x8") = number;
    __asm__ volatile("svc #0" : "+r"(a) : "r"(b), "r"(c), "r"(nr) : "memory", "cc");
#elif defined(__riscv)
    register uintptr_t a __asm__("a0") = first, b __asm__("a1") = second, c __asm__("a2") = third;
    register long nr __asm__("a7") = number;
    __asm__ volatile("ecall" : "+r"(a) : "r"(b), "r"(c), "r"(nr) : "memory");
#else
    register uintptr_t a __asm__("r0") = first, b __asm__("r1") = second, c __asm__("r2") = third;
    register long nr __asm__("r7") = number;
    __asm__ volatile("svc #0" : "+r"(a) : "r"(b), "r"(c), "r"(nr) : "memory", "cc");
#endif
    return (long)a;
}
void nos_dynamic_start(uintptr_t *stack) __attribute__((noreturn));
void nos_dynamic_start(uintptr_t *stack)
{
    unsigned valid = stack[0] == 2;
    uintptr_t *environment = stack + 1 + stack[0] + 1;
    while (*environment) environment++;
    uintptr_t *aux = environment + 1;
    unsigned found = 0;
    for (unsigned i = 0; i < 32 && aux[0]; i++, aux += 2) {
        if (aux[0] == 3 && aux[1]) found |= 1;
        if (aux[0] == 7 && aux[1]) found |= 2;
        if (aux[0] == 9 && aux[1]) found |= 4;
        if (aux[0] == 25 && aux[1]) found |= 8;
    }
    valid &= found == 15 && nos_dynamic_initialized == 1 && nos_dynamic_add(13) == 42;
    static const char pass[] = "PASS linux_dynamic_needed_constructor\n";
    static const char fail[] = "FAIL linux_dynamic_needed_constructor\n";
#if defined(__arm__)
    enum { SYS_WRITE = 4, SYS_EXIT = 1 };
#else
    enum { SYS_WRITE = 64, SYS_EXIT = 93 };
#endif
    raw_call(SYS_WRITE, 1, (uintptr_t)(valid ? pass : fail), valid ? sizeof(pass) - 1 : sizeof(fail) - 1);
    raw_call(SYS_EXIT, valid ? 0 : 73, 0, 0);
    for (;;) { }
}
