/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "thread.h"
#ifdef NOS_MUSL
#include <pthread.h>
#endif
#ifdef NOS_RISCV
static unsigned half_done, half_tid;
void nos_half_body(uintptr_t unused) __attribute__((used));
void nos_half_body(uintptr_t unused)
{
    (void)unused;
    __atomic_store_n(&half_done, 1, __ATOMIC_RELEASE);
    nos_syscall(NOS_SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
extern void nos_half_entry(uintptr_t unused);
__asm__(".pushsection .text.nos_half_entry,\"ax\"\n"
        ".balign 4\n.option push\n.option rvc\nc.nop\n"
        ".global nos_half_entry\nnos_half_entry:\nj nos_half_body\n"
        ".option pop\n.popsection\n");
static void halfword_regression(void)
{
    intptr_t stack = nos_syscall(NOS_SYS_MMAP, 3, 8192, 0);
    int ok = (uintptr_t)stack < (uintptr_t)-4095 && ((uintptr_t)nos_half_entry & 3) == 2;
    if (ok) {
        struct nos_thread_args args = { .entry = (uintptr_t)nos_half_entry,
            .stack = (uintptr_t)stack, .stack_size = 8192,
            .clear_tid = (uintptr_t)&half_tid, .parent_tid = (uintptr_t)&half_tid };
        ok &= nos_syscall(NOS_SYS_THREAD_CREATE, (uintptr_t)&args, UINTPTR_MAX, 0) >= 0;
        if (ok) {
            while (__atomic_load_n(&half_tid, __ATOMIC_ACQUIRE)) nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
            ok &= __atomic_load_n(&half_done, __ATOMIC_ACQUIRE) == 1;
        }
        ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)stack, 8192, 0);
    }
    nos_check(ok, "riscv_halfword_thread_entry");
}
#endif
#ifdef NOS_MMU
static void guard_worker(uintptr_t unused)
{
    (void)unused;
    *(volatile unsigned char *)0x0100c000u = 1;
    nos_syscall(NOS_SYS_EXIT, 99, 0, 0);
    for (;;) { }
}
#endif
#ifdef NOS_MPU
static void mpu_kernel_worker(uintptr_t unused)
{
    (void)unused;
    volatile unsigned char value = *(volatile unsigned char *)0x20000000u;
    nos_syscall(NOS_SYS_EXIT, value, 0, 0);
    for (;;) { }
}
static void mpu_write_worker(uintptr_t unused)
{
    (void)unused;
    intptr_t page = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ, NOS_PAGE_SIZE, 0);
    if (page > 0) *(volatile unsigned char *)(uintptr_t)page = 7;
    nos_syscall(NOS_SYS_EXIT, 99, 0, 0);
    for (;;) { }
}
static void mpu_execute_worker(uintptr_t unused)
{
    (void)unused;
    intptr_t page = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE, 0);
    if (page > 0) ((void (*)(void))((uintptr_t)page | 1u))();
    nos_syscall(NOS_SYS_EXIT, 99, 0, 0);
    for (;;) { }
}
static void mpu_regression(void)
{
    void (*workers[])(uintptr_t) = { mpu_kernel_worker, mpu_write_worker, mpu_execute_worker };
    const char *names[] = { "mpu_kernel_isolation", "mpu_readonly_mapping", "mpu_execute_never" };
    for (unsigned i = 0; i < 3; i++) {
        intptr_t status = 0;
        intptr_t child = nos_syscall(NOS_SYS_SPAWN, (uintptr_t)workers[i], 0, 8);
        nos_check(child >= 0 && !nos_syscall(NOS_SYS_WAIT, child, (uintptr_t)&status, 0) &&
            status == -NOS_EFAULT, names[i]);
    }
}
#endif
#if defined(NOS_MMU) && NOS_SMP > 1
static struct nos_lock smp_lock;
static unsigned smp_count, smp_mask, smp_gate, smp_ready, smp_active, smp_peak, smp_entered, smp_expected;
static void *smp_worker(void *unused)
{
    (void)unused;
    __atomic_add_fetch(&smp_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&smp_gate, __ATOMIC_ACQUIRE)) { }
    unsigned active = __atomic_add_fetch(&smp_active, 1, __ATOMIC_ACQ_REL);
    unsigned prior = __atomic_load_n(&smp_peak, __ATOMIC_RELAXED);
    while (prior < active && !__atomic_compare_exchange_n(&smp_peak, &prior, active, 0,
        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
    __atomic_add_fetch(&smp_entered, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&smp_entered, __ATOMIC_ACQUIRE) < smp_expected) { }
    for (unsigned i = 0; i < 100; i++) {
        unsigned cpu = (unsigned)nos_syscall(NOS_SYS_CPU_QUERY, 0, 0, 0);
        __atomic_fetch_or(&smp_mask, 1u << cpu, __ATOMIC_RELAXED);
        if (nos_lock_acquire(&smp_lock)) return (void *)1;
        smp_count++;
        if (nos_lock_release(&smp_lock)) return (void *)1;
        for (volatile unsigned delay = 0; delay < 500; delay++) { }
    }
    __atomic_sub_fetch(&smp_active, 1, __ATOMIC_RELEASE);
    return 0;
}
static unsigned tlb_ready, tlb_gate, tlb_survived, tlb_tid;
static uintptr_t tlb_address;
static void tlb_worker(uintptr_t unused)
{
    (void)unused;
    *(volatile unsigned char *)tlb_address = 19; /* Populate a writable TLB entry. */
    __atomic_store_n(&tlb_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&tlb_gate, __ATOMIC_ACQUIRE)) { }
    *(volatile unsigned char *)tlb_address = 29;
    __atomic_store_n(&tlb_survived, 1, __ATOMIC_RELEASE);
    nos_syscall(NOS_SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
static void smp_regression(void)
{
    unsigned count = (unsigned)nos_syscall(NOS_SYS_CPU_QUERY, 1, 0, 0), started = 0;
    int ok = count == NOS_SMP;
#ifdef NOS_MUSL
    pthread_t threads[NOS_SMP - 1];
#else
    struct nos_thread threads[NOS_SMP - 1];
#endif
    for (unsigned i = 0; i < NOS_SMP - 1; i++) {
#ifdef NOS_MUSL
        if (pthread_create(&threads[i], 0, smp_worker, 0)) break;
#else
        if (nos_thread_create(&threads[i], smp_worker, 0)) break;
#endif
        started++;
    }
    while (__atomic_load_n(&smp_ready, __ATOMIC_ACQUIRE) < started)
        nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
    smp_expected = started + 1;
    __atomic_store_n(&smp_gate, 1, __ATOMIC_RELEASE);
    smp_worker(0);
    for (unsigned i = 0; i < started; i++) {
        void *result = (void *)1;
#ifdef NOS_MUSL
        ok &= !pthread_join(threads[i], &result) && !result;
#else
        ok &= !nos_thread_join(&threads[i], &result) && !result;
#endif
    }
    nos_check(ok && started == NOS_SMP - 1 && smp_count == NOS_SMP * 100 &&
        smp_mask == ((1u << NOS_SMP) - 1) && smp_peak == NOS_SMP, "smp_parallel_shared_threads");
    intptr_t page = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE, 0);
    intptr_t stack = nos_syscall(NOS_SYS_MMAP, 3, 8192, 0);
    ok = page > 0 && stack > 0;
    if (ok) {
        tlb_address = (uintptr_t)page;
        struct nos_thread_args args = { .entry = (uintptr_t)tlb_worker, .stack = (uintptr_t)stack,
            .stack_size = 8192, .clear_tid = (uintptr_t)&tlb_tid, .parent_tid = (uintptr_t)&tlb_tid };
        ok &= nos_syscall(NOS_SYS_THREAD_CREATE, (uintptr_t)&args, UINTPTR_MAX, 0) >= 0;
        if (ok) {
            while (!__atomic_load_n(&tlb_ready, __ATOMIC_ACQUIRE)) nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
            ok &= !nos_syscall(NOS_SYS_MPROTECT, page, 1, NOS_PAGE_SIZE);
            __atomic_store_n(&tlb_gate, 1, __ATOMIC_RELEASE);
            while (__atomic_load_n(&tlb_tid, __ATOMIC_ACQUIRE)) nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
            ok &= !__atomic_load_n(&tlb_survived, __ATOMIC_ACQUIRE);
        }
    }
    if (stack > 0) ok &= !nos_syscall(NOS_SYS_MUNMAP, stack, 8192, 0);
    if (page > 0) ok &= !nos_syscall(NOS_SYS_MUNMAP, page, NOS_PAGE_SIZE, 0);
    nos_check(ok, "smp_tlb_shootdown");
    nos_puts("NOS CLOCK START\n");
    intptr_t before = nos_syscall(NOS_SYS_TICKS, 0, 0, 0);
    nos_syscall(NOS_SYS_SLEEP, 50, 0, 0);
    intptr_t after = nos_syscall(NOS_SYS_TICKS, 0, 0, 0);
    nos_puts("NOS CLOCK END\n");
    nos_check(after - before >= 50, "smp_clock_rate");
}
#endif
static const unsigned readonly_futex;
void nos_arch_regression(void)
{
    nos_check(nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&readonly_futex, 1, 0) == -NOS_EAGAIN,
        "futex_readonly_atomic_load");
#ifdef NOS_RISCV
    halfword_regression();
#endif
#ifdef NOS_MPU
    mpu_regression();
#endif
#if defined(NOS_MMU) && NOS_SMP > 1
    smp_regression();
#endif
#ifdef NOS_MMU
    intptr_t status = 0;
    intptr_t child = nos_syscall(NOS_SYS_SPAWN, (uintptr_t)guard_worker, 0, 8);
    nos_check(child >= 0 && !nos_syscall(NOS_SYS_WAIT, child, (uintptr_t)&status, 0) &&
        status == -NOS_EFAULT, "user_stack_guard");
    struct nos_object_stats before, after;
    int ok = !nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0);
    intptr_t mapped = nos_syscall(NOS_SYS_MMAP, 3, 3 * NOS_PAGE_SIZE, 0);
    if (mapped > 0) {
        uintptr_t base = (uintptr_t)mapped, middle = base + NOS_PAGE_SIZE;
        *(volatile unsigned char *)base = 11;
        *(volatile unsigned char *)(base + 2 * NOS_PAGE_SIZE) = 22;
        ok &= !nos_syscall(NOS_SYS_MPROTECT, middle, 1, NOS_PAGE_SIZE);
        ok &= nos_syscall(NOS_SYS_USER_CHECK, middle, 1, 2) == -NOS_EFAULT;
        ok &= !nos_syscall(NOS_SYS_MPROTECT, middle, 3, NOS_PAGE_SIZE);
        ok &= !nos_syscall(NOS_SYS_MUNMAP, middle, NOS_PAGE_SIZE, 0);
        ok &= nos_syscall(NOS_SYS_USER_CHECK, middle, 1, 1) == -NOS_EFAULT;
        ok &= *(volatile unsigned char *)base == 11 && *(volatile unsigned char *)(base + 2 * NOS_PAGE_SIZE) == 22;
        ok &= !nos_syscall(NOS_SYS_MUNMAP, base, NOS_PAGE_SIZE, 0);
        ok &= !nos_syscall(NOS_SYS_MUNMAP, base + 2 * NOS_PAGE_SIZE, NOS_PAGE_SIZE, 0);
    } else ok = 0;
    ok &= !nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0) && before.free_pages == after.free_pages;
    nos_check(ok, "partial_vma_operations");
    mapped = nos_syscall(NOS_SYS_MMAP, 0, NOS_PAGE_SIZE, 0);
    ok = mapped > 0 && nos_syscall(NOS_SYS_USER_CHECK, mapped, 1, 1) == -NOS_EFAULT &&
        !nos_syscall(NOS_SYS_MPROTECT, mapped, 3, NOS_PAGE_SIZE);
    if (ok) *(volatile unsigned char *)(uintptr_t)mapped = 37;
    ok &= mapped > 0 && !nos_syscall(NOS_SYS_MUNMAP, mapped, NOS_PAGE_SIZE, 0);
    nos_check(ok, "prot_none_mapping");
#endif
}
