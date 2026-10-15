/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
void *k_memcpy(void *dst, const void *src, size_t count)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (count--) *d++ = *s++;
    return dst;
}
void *k_memset(void *dst, int value, size_t count)
{
    unsigned char *d = dst;
    while (count--) *d++ = (unsigned char)value;
    return dst;
}
void kernel_print(const char *text)
{
    while (*text) arch_putc(*text++);
}
void kernel_fail(const char *text)
{
#if defined(NOS_KERNEL_FAULT_INJECT) && NOS_KERNEL_FAULT_INJECT
    const char *expected = "privileged_exception", *actual = text;
    while (*expected && *expected == *actual) { expected++; actual++; }
    if (!*expected && !*actual) kernel_print("PANIC privileged_exception\n");
#endif
    kernel_print("FAIL kernel: ");
    kernel_print(text);
    kernel_print("\n");
    arch_shutdown(1);
}
uintptr_t spin_lock_irqsave(struct spinlock *lock)
{
    uintptr_t saved = arch_irq_save();
    while (__atomic_exchange_n(&lock->value, 1u, __ATOMIC_ACQUIRE)) { }
    return saved;
}
void spin_unlock_irqrestore(struct spinlock *lock, uintptr_t saved)
{
    __atomic_store_n(&lock->value, 0u, __ATOMIC_RELEASE);
    arch_irq_restore(saved);
}
void *k_memmove(void *dst, const void *src, size_t count)
{
    unsigned char *d = dst; const unsigned char *s = src;
    if ((uintptr_t)d > (uintptr_t)s && (uintptr_t)d - (uintptr_t)s < count) {
        while (count) { count--; d[count] = s[count]; }
    } else k_memcpy(dst, src, count);
    return dst;
}
int k_memcmp(const void *a, const void *b, size_t count)
{
    const unsigned char *x = a, *y = b;
    while (count--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}
#if defined(NOS_ARM) || defined(NOS_ARM_M)
#define AEABI_MEMORY(suffix) \
void nos_kernel___aeabi_memcpy##suffix(void *d, const void *s, size_t n) { k_memcpy(d, s, n); } \
void nos_kernel___aeabi_memmove##suffix(void *d, const void *s, size_t n) { k_memmove(d, s, n); } \
void nos_kernel___aeabi_memset##suffix(void *d, size_t n, int c) { k_memset(d, c, n); } \
void nos_kernel___aeabi_memclr##suffix(void *d, size_t n) { k_memset(d, 0, n); }
AEABI_MEMORY()
AEABI_MEMORY(4)
AEABI_MEMORY(8)
#endif
void nos_kernel_abort(void) { kernel_fail("compiler runtime abort"); }
int nos_kernel_raise(int signal) { (void)signal; kernel_fail("compiler runtime exception"); }
