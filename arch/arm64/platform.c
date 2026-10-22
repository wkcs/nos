/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
struct tables { uint64_t root[512], low[512], user[NOS_USER_PT_SLOTS][512];
    unsigned short binding[NOS_USER_PT_SLOTS], live[NOS_USER_PT_SLOTS];
} __attribute__((aligned(4096)));
static struct tables spaces[NOS_SPACES] __attribute__((aligned(4096)));
static int enabled[NOS_CPUS];
static unsigned kernel_tables_ready;
static uint64_t kernel_low[512] __attribute__((aligned(4096)));
static uint64_t kernel_pages[64][512] __attribute__((aligned(4096)));
extern char __kernel_start[], __kernel_ro_end[], kernel_stack_guard[];
static unsigned period, claimed[NOS_CPUS];
static volatile uint32_t *reg(uintptr_t p) { return (volatile uint32_t *)p; }
void arch_platform_init(void) { }
void arch_putc(char c)
{
    while (*reg(0x09000018) & 0x20) { }
    *reg(0x09000000) = (unsigned char)c;
}
void arch_shutdown(int failed)
{
    uintptr_t args[2] = { 0x20026, (uintptr_t)failed };
    register uintptr_t x0 __asm__("x0") = 0x20;
    register uintptr_t x1 __asm__("x1") = (uintptr_t)args;
    __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory");
    for (;;) __asm__ volatile("wfi");
}
uintptr_t arch_irq_save(void)
{
    uintptr_t v;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(v) :: "memory");
    return v;
}
void arch_irq_restore(uintptr_t v) { __asm__ volatile("msr daif, %0" :: "r"(v) : "memory"); }
static void rearm(void)
{
    uint64_t p = period, ctl = 1;
    __asm__ volatile("msr cntp_tval_el0, %0; msr cntp_ctl_el0, %1; isb" :: "r"(p), "r"(ctl));
}
void arch_timer_init(void)
{
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    period = (unsigned)(freq / 100);
    *reg(0x08000000) = 1;
    *reg(0x08000100) = 1u << 30;
    *reg(0x0800041c) = 0x80808080;
    *reg(0x08010004) = 0xff;
    *reg(0x08010000) = 1;
    rearm();
}
void arch_timer_ack(void)
{
    rearm();
    *reg(0x08010010) = claimed[arch_cpu_id()];
}
static uint64_t table(void *p) { return (uintptr_t)p | 3; }
void arch_space_init(unsigned id)
{
    struct tables *t = &spaces[id];
    k_memset(t, 0, sizeof(*t));
    for (unsigned n = 0; n < NOS_USER_PT_SLOTS; n++) t->binding[n] = 0xffff;
    t->root[0] = table(t->low);
    /* Shared privileged tables keep executable pages read-only and all
     * physical heap/data aliases non-executable. */
    if (!kernel_tables_ready) {
    for (unsigned block = 0; block < 512; block++)
        kernel_low[block] = (0x40000000ull + (uint64_t)block * 0x200000) | 1 |
            (1ull << 10) | (3ull << 8) | (1ull << 2) | (1ull << 53) | (1ull << 54);
    for (unsigned block = 0; block < 64; block++) {
        kernel_low[block] = table(kernel_pages[block]);
        for (unsigned page = 0; page < 512; page++) {
            uintptr_t pa = 0x40000000ull + block * 0x200000 + page * 4096;
            uint64_t bits = 3 | (1ull << 10) | (3ull << 8) | (1ull << 2) | (1ull << 54);
            if (pa >= (uintptr_t)__kernel_start && pa < (uintptr_t)__kernel_ro_end) bits |= 1ull << 7;
            else bits |= 1ull << 53;
            kernel_pages[block][page] = (pa >= (uintptr_t)kernel_stack_guard && pa - (uintptr_t)kernel_stack_guard < NOS_SMP * 32768u &&
                ((pa - (uintptr_t)kernel_stack_guard) & 32767) < 4096) ? 0 : pa | bits;
        }
    }
        kernel_tables_ready = 1;
    }
    t->root[1] = table(kernel_low);
    t->low[64] = 0x08000000ull | 1 | (1ull << 10) | (1ull << 53) | (1ull << 54);
    t->low[80] = 0x0a000000ull | 1 | (1ull << 10) | (1ull << 53) | (1ull << 54);
    t->low[72] = 0x09000000ull | 1 | (1ull << 10) | (1ull << 53) | (1ull << 54);

}
static uint64_t *user_pte(struct tables *t, uintptr_t va, int create, unsigned *slot)
{
    if (va < VM_USER_BASE || va >= VM_USER_END) return NULL;
    unsigned block = (unsigned)(va >> 21), free_slot = NOS_USER_PT_SLOTS;
    if ((t->low[block] & 3) == 1) return NULL; /* Never replace a privileged device block. */
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
    uint64_t bits = 3 | (1ull << 10) | (3ull << 8) | (1ull << 2);
    if (!(flags & ARCH_MAP_PRIVILEGED)) bits |= 1ull << 6;
    if (!(flags & NOS_PROT_WRITE)) bits |= 1ull << 7;
    if (!(flags & NOS_PROT_EXEC)) bits |= (1ull << 54) | (1ull << 53);
    *p = flags ? pa | bits : 0;
    return 0;
}
void arch_unmap(unsigned id, uintptr_t va)
{ (void)arch_map(id, va, 0, 0); }
void arch_tlb_sync(void)
{ retire_user_tables(0); __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory"); retire_user_tables(1); }
void arch_cache_sync(void)
{ __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory"); }
unsigned arch_cpu_id(void) { uintptr_t id; __asm__ volatile("mrs %0, mpidr_el1" : "=r"(id)); return (unsigned)(id & 255); }
unsigned arch_cpu_count(void) { return NOS_SMP; }
extern void secondary_start(void);
void arch_start_secondary(void)
{
    for (unsigned cpu = 1; cpu < NOS_SMP; cpu++) {
        register uintptr_t x0 __asm__("x0") = 0xc4000003u;
        register uintptr_t x1 __asm__("x1") = cpu;
        register uintptr_t x2 __asm__("x2") = (uintptr_t)secondary_start;
        register uintptr_t x3 __asm__("x3") = 0;
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
        if ((intptr_t)x0) kernel_fail("PSCI CPU_ON failed");
    }
}
void arch_secondary_init(void) { }
void arch_space_switch(unsigned id)
{
    uintptr_t ttbr = (uintptr_t)spaces[id].root;
    __asm__ volatile("dsb sy; msr ttbr0_el1, %0; tlbi vmalle1; dsb sy; isb" :: "r"(ttbr) : "memory");
    if (!enabled[arch_cpu_id()]) {
        uint64_t mair = 0xff00, tcr = 25 | (1u << 8) | (1u << 10) | (3u << 12);
        uint64_t sctlr;
        __asm__ volatile("msr mair_el1, %0; msr tcr_el1, %1; isb" :: "r"(mair), "r"(tcr));
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        sctlr |= 1;
        __asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr) : "memory");
        enabled[arch_cpu_id()] = 1;
    }
}
void arch_frame_init(struct arch_frame *f, uintptr_t pc, uintptr_t sp, uintptr_t arg, uintptr_t second)
{
    k_memset(f, 0, sizeof(*f)); f->pc = pc; f->sp = sp; f->x[0] = arg; f->x[1] = second;
}
struct arch_frame *arch_return_frame(struct arch_frame *f) { return f; }

intptr_t arch_device_operation(struct task *task, uintptr_t op, uintptr_t b, uintptr_t c)
{
    if (op == 5) { unsigned status = *reg(0x09000018); return (!(status & 0x10) ? 1 : 0) | (!(status & 0x20) ? 4 : 0); }
    if (op == 4) { *reg(0x09000038) = b ? ((1u << 4) | (1u << 6)) : 0; return 0; }
    if (!op) {
        if (*reg(0x09000018) & 0x10) { *reg(0x09000044) = 1u << 6; return -NOS_EAGAIN; }
        return (intptr_t)(*reg(0x09000000) & 255);
    }
    if (op == 1) return b < 32 ? (intptr_t)(0x0a000000u + b * 0x200u) : -NOS_EINVAL;
    if (op == 2) {
        if (b < 0x0a000000u || b >= 0x0a004000u || (b & 4095) ||
            c < 0x03000000u || c >= 0x03200000u || (c & 4095)) return -NOS_EINVAL;
        struct tables *t = &spaces[task->space_id]; unsigned slot;
        uint64_t *pte = user_pte(t, c, 1, &slot);
        if (!pte) return -NOS_ENOMEM;
        if (!*pte) t->live[slot]++;
        *pte = b | 3 | (1ull << 10) | (1ull << 6) | (1ull << 53) | (1ull << 54);
        arch_tlb_sync(); return (intptr_t)c;
    }
    if (op == 3) { uintptr_t pa = task_user_phys(task, b); return pa ? (intptr_t)pa : -NOS_EFAULT; }
    return -NOS_ENOTSUP;
}

/* PL031 data register reports UTC seconds.  Its page is privileged in
 * the existing UART device block; user mapping APIs exclude that block. */
unsigned arch_fault_signal(void)
{
    uintptr_t status; __asm__ volatile("mrs %0, esr_el1" : "=r"(status));
    unsigned cause = (unsigned)(status >> 26);
    if (cause == 0x2c) return 8;
    if (cause == 0x3c) return 5;
    if (cause == 0x22 || cause == 0x26 || ((cause == 0x24 || cause == 0x25) && (status & 63) == 0x21)) return 7;
    if (cause == 0x20 || cause == 0x21 || cause == 0x24 || cause == 0x25) return 11;
    return 4;
}

unsigned arch_fault_access(void)
{
    uintptr_t status; __asm__ volatile("mrs %0, esr_el1" : "=r"(status));
    unsigned cause = (unsigned)(status >> 26), code = (unsigned)status & 63;
    if (code < 4 || code > 7) return 0; /* Translation faults, levels 0..3. */
    if (cause == 0x20) return NOS_PROT_EXEC;
    if (cause != 0x24) return 0;
    return status & (1u << 6) ? NOS_PROT_WRITE : NOS_PROT_READ;
}

uintptr_t arch_fault_address(void)
{
    uintptr_t address;
    __asm__ volatile("mrs %0, far_el1" : "=r"(address));
    return address;
}

uint64_t arch_realtime_ns(void)
{
#ifdef NOS_ARM_M
    return 0;
#else
    return (uint64_t)*reg(0x09010000u) * 1000000000ull;
#endif
}

void arch_device_revoke(struct task *task)
{
    if (!(task->caps & NOS_CAP_DEVICE)) return;
    *reg(0x09000038) = 0; arch_irq_mask(33, 1);
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
    unsigned irq = *reg(0x0801000c); claimed[arch_cpu_id()] = irq;
    return (irq & 1023) == 30 ? 0 : irq & 1023;
}
void arch_irq_complete(unsigned irq)
{ (void)irq; *reg(0x08010010) = claimed[arch_cpu_id()]; }
void arch_irq_mask(unsigned irq, int masked)
{
    if (irq >= 1020) return;
    *reg(0x08000000u + (masked ? 0x180u : 0x100u) + (irq / 32) * 4) = 1u << (irq % 32);
    __asm__ volatile("dsb sy" ::: "memory");
}
int arch_irq_bind(unsigned device_index)
{
    if (device_index >= 32 && device_index != 0x100) return -NOS_EINVAL;
    unsigned irq = device_index == 0x100 ? 33 : 48 + device_index;
    *(volatile unsigned char *)(uintptr_t)(0x08000800u + irq) = 1;
    *(volatile unsigned char *)(uintptr_t)(0x08000400u + irq) = 0x80;
    arch_irq_mask(irq, 0); return (int)irq;
}

unsigned arch_mapping_flags(unsigned id, uintptr_t va)
{
    if (id >= NOS_SPACES) return 0;
    uint64_t *p = user_pte(&spaces[id], va, 0, NULL);
    if (!p || !*p) return 0;
    uint64_t entry = *p;
    return 1 | (!(entry & (1ull << 7)) ? 2 : 0) | (!(entry & (1ull << 54)) ? 4 : 0);
}
void arch_fault_probe(void)
{ __asm__ volatile("brk #0" ::: "memory"); }
