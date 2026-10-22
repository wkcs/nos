/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
struct tables { uint64_t root[512], low[512], user[NOS_USER_PT_SLOTS][512];
    unsigned short binding[NOS_USER_PT_SLOTS], live[NOS_USER_PT_SLOTS];
} __attribute__((aligned(4096)));
static struct tables spaces[NOS_SPACES] __attribute__((aligned(4096)));
static unsigned kernel_tables_ready;
static uint64_t kernel_low[512] __attribute__((aligned(4096)));
static uint64_t kernel_pages[64][512] __attribute__((aligned(4096)));
extern char __kernel_start[], __kernel_ro_end[], kernel_stack_guard[];
volatile uintptr_t secondary_release;
volatile unsigned tlb_ack[NOS_CPUS];
void arch_platform_init(void) { }
void arch_putc(char c)
{
    volatile unsigned char *uart = (void *)0x10000000;
    while (!(uart[5] & 0x20)) { }
    uart[0] = (unsigned char)c;
}
void arch_shutdown(int failed)
{
    *(volatile uint32_t *)0x100000 = failed ? 0x13333 : 0x5555;
    for (;;) __asm__ volatile("wfi");
}
uintptr_t arch_irq_save(void)
{
    uintptr_t v; __asm__ volatile("csrrc %0, sstatus, %1" : "=r"(v) : "r"((uintptr_t)2) : "memory"); return v;
}
void arch_irq_restore(uintptr_t v) { if (v & 2) __asm__ volatile("csrs sstatus, %0" :: "r"((uintptr_t)2) : "memory"); }
void arch_timer_init(void) { __asm__ volatile("csrs sie, %0" :: "r"((uintptr_t)512)); }
void arch_timer_ack(void) { __asm__ volatile("csrc sip, %0" :: "r"((uintptr_t)2)); }
static uint64_t table(void *p) { return (((uintptr_t)p >> 12) << 10) | 1; }
static uint64_t leaf(uintptr_t pa, unsigned flags, int user)
{
    uint64_t bits = 1 | 64 | 128;
    if (flags & NOS_PROT_READ) bits |= 2;
    if (flags & NOS_PROT_WRITE) bits |= 4;
    if (flags & NOS_PROT_EXEC) bits |= 8;
    if (user) bits |= 16;
    return ((pa >> 12) << 10) | bits;
}
void arch_space_init(unsigned id)
{
    struct tables *t = &spaces[id]; k_memset(t, 0, sizeof(*t));
    for (unsigned n = 0; n < NOS_USER_PT_SLOTS; n++) t->binding[n] = 0xffff;
    if (!kernel_tables_ready) {
    for (unsigned block = 0; block < 512; block++) kernel_low[block] = leaf(0x80000000ull + (uint64_t)block * 0x200000, 3, 0);
    for (unsigned block = 0; block < 64; block++) {
        kernel_low[block] = table(kernel_pages[block]);
        for (unsigned page = 0; page < 512; page++) {
            uintptr_t pa = 0x80000000ull + block * 0x200000 + page * 4096;
            unsigned prot = pa >= (uintptr_t)__kernel_start && pa < (uintptr_t)__kernel_ro_end ? 5 : 3;
            kernel_pages[block][page] = (pa >= (uintptr_t)kernel_stack_guard && pa - (uintptr_t)kernel_stack_guard < NOS_SMP * 32768u &&
                ((pa - (uintptr_t)kernel_stack_guard) & 32767) < 4096) ? 0 : leaf(pa, prot, 0);
        }
    }
        kernel_tables_ready = 1;
    }
    t->root[2] = table(kernel_low);
    t->root[0] = table(t->low);
    t->low[0] = leaf(0, 3, 0);
    t->low[32] = leaf(0x02000000u, 3, 0); /* CLINT supervisor alias */
    for (unsigned i = 96; i < 100; i++) t->low[i] = leaf((uintptr_t)i * 0x200000u, 3, 0);
    t->low[128] = leaf(0x10000000, 3, 0);

}
static uint64_t *user_pte(struct tables *t, uintptr_t va, int create, unsigned *slot)
{
    if (va < VM_USER_BASE || va >= VM_USER_END) return NULL;
    unsigned block = (unsigned)(va >> 21), free_slot = NOS_USER_PT_SLOTS;
    if ((t->low[block] & 14) != 0) return NULL; /* Never replace a privileged device block. */
    for (unsigned n = 0; n < NOS_USER_PT_SLOTS; n++) {
        if (t->binding[n] == block) { if (slot) *slot = n; return &t->user[n][(va >> 12) & 511]; }
        if (t->binding[n] == 0xffff && free_slot == NOS_USER_PT_SLOTS) free_slot = n;
    }
    if (!create || free_slot == NOS_USER_PT_SLOTS) return NULL;
    k_memset(t->user[free_slot], 0, sizeof(t->user[free_slot]));
    t->binding[free_slot] = (unsigned short)block;
    t->low[block] = table(t->user[free_slot]);
    if (slot) *slot = free_slot;
    return &t->user[free_slot][(va >> 12) & 511];
}
/* Empty tables retire before the broadcast; their backing storage is reusable
 * only after all CPUs have completed the TLB invalidation. */
static void retire_user_tables(int finish)
{
    for (unsigned id = 0; id < NOS_SPACES; id++) for (unsigned n = 0; n < NOS_USER_PT_SLOTS; n++) {
        struct tables *t = &spaces[id];
        if (t->binding[n] == 0xffff || t->live[n]) continue;
        if (!finish) t->low[t->binding[n]] = 0;
        else t->binding[n] = 0xffff;
    }
}
int arch_map(unsigned id, uintptr_t va, uintptr_t pa, unsigned flags)
{
    struct tables *t = &spaces[id];
    if ((va & 4095) || (pa & 4095) || ((flags & 6) == 6)) return -NOS_EINVAL;
    unsigned slot = 0;
    uint64_t *p = user_pte(t, va, !!flags, &slot);
    if (!p) return flags ? -NOS_ENOMEM : 0;
    if (!*p && flags) t->live[slot]++;
    else if (*p && !flags) t->live[slot]--;
    *p = flags ? leaf(pa, flags, !(flags & ARCH_MAP_PRIVILEGED)) : 0;
    return 0;
}
void arch_unmap(unsigned id, uintptr_t va) { (void)arch_map(id, va, 0, 0); }
static void remote_sync(void)
{
    if (!secondary_release) return;
    for (unsigned cpu = 0; cpu < NOS_SMP; cpu++) {
        if (cpu == arch_cpu_id()) continue;
        tlb_ack[cpu] = 0;
        __asm__ volatile("fence rw, rw" ::: "memory");
        *(volatile uint32_t *)(uintptr_t)(0x04000000u + cpu * 4) = 1;
        while (!tlb_ack[cpu]) { __asm__ volatile("nop" ::: "memory"); }
        __asm__ volatile("fence rw, rw" ::: "memory");
    }
}
void arch_tlb_sync(void) { retire_user_tables(0); __asm__ volatile("sfence.vma" ::: "memory"); remote_sync(); retire_user_tables(1); }
void arch_cache_sync(void) { __asm__ volatile("fence.i" ::: "memory"); remote_sync(); }
unsigned arch_cpu_id(void) { uintptr_t id; __asm__ volatile("mv %0, tp" : "=r"(id)); return (unsigned)id; }
unsigned arch_cpu_count(void) { return NOS_SMP; }
void arch_start_secondary(void)
{ __asm__ volatile("fence rw, rw" ::: "memory"); secondary_release = 1; }
void arch_secondary_init(void) { }
void arch_space_switch(unsigned id)
{
    uintptr_t satp = (8ull << 60) | ((uintptr_t)spaces[id].root >> 12);
    __asm__ volatile("csrw satp, %0; sfence.vma" :: "r"(satp) : "memory");
}
void arch_frame_init(struct arch_frame *f, uintptr_t pc, uintptr_t sp, uintptr_t arg, uintptr_t second)
{
    k_memset(f, 0, sizeof(*f)); f->pc = pc; f->x[2] = sp; f->x[10] = arg; f->x[11] = second; f->status = (1u << 5) | (3u << 13);
}
struct arch_frame *arch_return_frame(struct arch_frame *f) { return f; }

intptr_t arch_device_operation(struct task *task, uintptr_t op, uintptr_t b, uintptr_t c)
{
    volatile unsigned char *uart = (void *)0x10000000;
    if (op == 5) { unsigned status = uart[5]; return ((status & 1) ? 1 : 0) | ((status & 0x20) ? 4 : 0); }
    if (op == 4) { uart[1] = b ? 1 : 0; return 0; }
    if (!op) return (uart[5] & 1) ? uart[0] : -NOS_EAGAIN;
    if (op == 1) return b < 8 ? (intptr_t)(0x10001000u + b * 0x1000u) : -NOS_EINVAL;
    if (op == 2) {
        if (b < 0x10001000u || b >= 0x10009000u || (b & 4095) ||
            c < 0x03000000u || c >= 0x03200000u || (c & 4095)) return -NOS_EINVAL;
        struct tables *t = &spaces[task->space_id]; unsigned slot;
        uint64_t *pte = user_pte(t, c, 1, &slot);
        if (!pte) return -NOS_ENOMEM;
        if (!*pte) t->live[slot]++;
        *pte = leaf(b, 3, 1);
        arch_tlb_sync(); return (intptr_t)c;
    }
    if (op == 3) { uintptr_t pa = task_user_phys(task, b); return pa ? (intptr_t)pa : -NOS_EFAULT; }
    return -NOS_ENOTSUP;
}

unsigned arch_fault_signal(void)
{
    uintptr_t cause; __asm__ volatile("csrr %0, scause" : "=r"(cause));
    if (cause == 2) return 4;
    if (cause == 3) return 5;
    if (cause == 0 || cause == 4 || cause == 6) return 7;
    return 11;
}

unsigned arch_fault_access(void)
{
    uintptr_t cause; __asm__ volatile("csrr %0, scause" : "=r"(cause));
    if (cause == 12) return NOS_PROT_EXEC;
    if (cause == 13) return NOS_PROT_READ;
    if (cause == 15) return NOS_PROT_WRITE;
    return 0;
}

uintptr_t arch_fault_address(void)
{
    uintptr_t address;
    __asm__ volatile("csrr %0, stval" : "=r"(address));
    return address;
}

uint64_t arch_realtime_ns(void)
{
    /* Goldfish TIME_LOW latches TIME_HIGH until the following high read. */
    volatile uint32_t *rtc = (void *)(uintptr_t)0x00101000u;
    uint32_t low = rtc[0];
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    uint32_t high = rtc[1];
    return ((uint64_t)high << 32) | low;
}

void arch_device_revoke(struct task *task)
{
    if (!(task->caps & NOS_CAP_DEVICE)) return;
    *(volatile unsigned char *)(uintptr_t)0x10000001u = 0;
    arch_irq_mask(10, 1);
#ifdef NOS_ARM_M
    return;
#else
    unsigned count =
#ifdef NOS_RISCV
        8;
    uintptr_t base = 0x10001000u, stride = 0x1000u;
#else
        32;
    uintptr_t base = 0x0a000000u, stride = 0x200u;
#endif
    for (unsigned i = 0; i < count; i++) {
        volatile uint32_t *mmio = (void *)(base + i * stride);
        if (mmio[0] != 0x74726976 || (mmio[2] != 2 && mmio[2] != 4)) continue;
        mmio[0x70 / 4] = 0;
#ifdef NOS_RISCV
        __asm__ volatile("fence iorw, iorw" ::: "memory");
#else
        __asm__ volatile("dsb sy" ::: "memory");
#endif
        unsigned remaining = 100000;
        while (mmio[0x70 / 4] && remaining) remaining--;
        if (!remaining) kernel_fail("device reset did not quiesce DMA");
    }
#endif
}

unsigned arch_irq_claim(void)
{
    uintptr_t cause; __asm__ volatile("csrr %0, scause" : "=r"(cause));
    if ((cause & 255) == 9)
        return *(volatile uint32_t *)(uintptr_t)(0x0c201004u + arch_cpu_id() * 0x2000u);
    return 0;
}
void arch_irq_complete(unsigned irq)
{ *(volatile uint32_t *)(uintptr_t)(0x0c201004u + arch_cpu_id() * 0x2000u) = irq; }
void arch_irq_mask(unsigned irq, int masked)
{
    if (!irq || irq > 31) return;
    volatile uint32_t *enable = (void *)(uintptr_t)0x0c002080u;
    if (masked) *enable &= ~(1u << irq); else *enable |= 1u << irq;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    if (!masked) {
        /* IRQs route to hart 0's supervisor context. Stock QEMU 10.0.13
         * does not re-evaluate pending IRQs on an enable-register write.
         * Preserve the threshold and rewrite it to re-evaluate the source
         * that may have become pending while the receiver drained UART. */
        volatile uint32_t *threshold = (void *)(uintptr_t)0x0c201000u;
        uint32_t priority = *threshold;
        *threshold = priority;
        __asm__ volatile("fence iorw, iorw" ::: "memory");
    }
}
int arch_irq_bind(unsigned device_index)
{
    if (device_index >= 8 && device_index != 0x100) return -NOS_EINVAL;
    unsigned irq = device_index == 0x100 ? 10 : 1 + device_index;
    *(volatile uint32_t *)(uintptr_t)(0x0c000000u + irq * 4) = 1;
    *(volatile uint32_t *)(uintptr_t)0x0c201000u = 0;
    arch_irq_mask(irq, 0);
    __asm__ volatile("csrs sie, %0" :: "r"((uintptr_t)512) : "memory");
    return (int)irq;
}

unsigned arch_mapping_flags(unsigned id, uintptr_t va)
{
    if (id >= NOS_SPACES) return 0;
    uint64_t *p = user_pte(&spaces[id], va, 0, NULL);
    if (!p || !*p) return 0;
    uint64_t entry = *p;
#ifdef NOS_LINUX_COMPAT
    /* A page-fault syndrome also covers permission faults. Fault retries
     * require a valid accessed user leaf, not merely plausible RWX bits. */
    if ((entry & (1u | 16u | 64u)) != (1u | 16u | 64u) || !(entry & 14u) ||
        ((entry & 4u) && !(entry & 2u))) return 0;
    unsigned flags = (unsigned)((entry >> 1) & 7u);
    if (!(entry & 128u)) flags &= ~NOS_PROT_WRITE;
    return flags;
#else
    return (unsigned)((entry >> 1) & 7);
#endif
}
void arch_fault_probe(void)
{ __asm__ volatile(".word 0" ::: "memory"); }
