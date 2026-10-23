/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
#ifdef NOS_NK60_V2
#include "nk60_device.h"
#endif
static volatile uint32_t *reg(uintptr_t p) { return (volatile uint32_t *)p; }
#ifdef NOS_ARM_M
void arch_platform_init(void)
{
#ifdef NOS_NK60_V2
    extern void nk60_boot_init(void);
    nk60_boot_init();
#endif
    *reg(0x4001100c) = (1u << 13) | (1u << 3) | (1u << 2);
#ifdef NOS_MPU
    /* Enable MemManage rather than allowing all MPU violations to escalate. */
    *reg(0xe000ed24) |= 1u << 16;
    if (!(*reg(0xe000ed90) & 0xff00)) kernel_fail("CPU has no MPU");
#endif
}
void arch_putc(char c)
{
#ifdef NOS_NK60_V2
    unsigned remaining = 1000000u;
    while (!(*reg(0x40011000u) & (1u << 7)) && --remaining) { }
    if (!remaining) return;
#endif
    *reg(0x40011004) = (unsigned char)c;
}
uintptr_t arch_irq_save(void)
{
    uintptr_t v; __asm__ volatile("mrs %0, primask; cpsid i" : "=r"(v) :: "memory"); return v;
}
void arch_irq_restore(uintptr_t v) { __asm__ volatile("msr primask, %0" :: "r"(v) : "memory"); }
void arch_timer_init(void)
{
#ifdef NOS_NK60_V2
    *reg(0xe000e014) = 840000u - 1u; /* 84 MHz / 100 Hz */
#else
    *reg(0xe000e014) = 1680000 - 1;
#endif
    *reg(0xe000e018) = 0;
    *reg(0xe000e010) = 7;
}
void arch_timer_ack(void) { (void)*reg(0xe000e010); }
void arch_space_init(unsigned id) { (void)id; }
#ifdef NOS_MPU
extern unsigned task_mpu_regions(struct task *, uintptr_t *, size_t *, unsigned *);
static int mpu_layout(uintptr_t base, size_t size, uintptr_t *region_base, unsigned *rasr)
{
    if (!size || base > UINTPTR_MAX - size) return 0;
    for (unsigned exponent = 8; exponent < 32; exponent++) {
        uintptr_t span = (uintptr_t)1 << exponent, start = base & ~(span - 1);
        uintptr_t granule = span / 8;
        if (base + size - start > span || (base & (granule - 1)) || (size & (granule - 1))) continue;
        unsigned first = (unsigned)((base - start) / granule), count = (unsigned)(size / granule);
        unsigned mask = (unsigned)(((1u << count) - 1u) << first);
        *region_base = start; *rasr = 1 | ((exponent - 1) << 1) | ((255u ^ mask) << 8);
        return 1;
    }
    return 0;
}
int arch_mpu_valid_range(uintptr_t base, size_t size)
{ uintptr_t region_base; unsigned rasr; return mpu_layout(base, size, &region_base, &rasr); }
static void mpu_region(unsigned index, uintptr_t base, size_t size, unsigned prot)
{
    uintptr_t region_base; unsigned rasr;
    if (!mpu_layout(base, size, &region_base, &rasr)) kernel_fail("invalid MPU mapping committed");
    *reg(0xe000ed98) = index;
    *reg(0xe000ed9c) = (uint32_t)region_base;
    *reg(0xe000eda0) = rasr | (1u << 17) | (1u << 16) |
        ((prot & NOS_PROT_WRITE ? 3u : 6u) << 24) | (prot & NOS_PROT_EXEC ? 0u : 1u << 28);
}
#endif
void arch_space_switch(unsigned id)
{
    (void)id;
#ifdef NOS_MPU
    uintptr_t bases[7]; size_t sizes[7]; unsigned flags[7];
    unsigned n = task_mpu_regions(current_task, bases, sizes, flags);
    *reg(0xe000ed94) = 0;
#ifdef NOS_NK60_V2
    mpu_region(0, 0x08000000u, 256u * 1024u, 5);
#else
    mpu_region(0, 0x08000000u, 1024 * 1024, 5);
#endif
    for (unsigned i = 0; i < n; i++) mpu_region(i + 1, bases[i], sizes[i], flags[i]);
    for (unsigned i = n + 1; i < 8; i++) { *reg(0xe000ed98) = i; *reg(0xe000eda0) = 0; }
    *reg(0xe000ed94) = 5; /* enable, privileged background mapping */
    __asm__ volatile("dsb; isb" ::: "memory");
#endif
}
void arch_tlb_sync(void) { __asm__ volatile("dsb; isb" ::: "memory"); }
void arch_cache_sync(void) { __asm__ volatile("dsb; isb" ::: "memory"); }
unsigned arch_cpu_id(void) { return 0; }
unsigned arch_cpu_count(void) { return 1; }
void arch_start_secondary(void) { }
void arch_secondary_init(void) { arch_timer_init(); }
int arch_map(unsigned id, uintptr_t va, uintptr_t pa, unsigned flags)
{ (void)id; (void)va; (void)pa; (void)flags; return 0; }
void arch_unmap(unsigned id, uintptr_t va) { (void)id; (void)va; }
void arch_frame_init(struct arch_frame *f, uintptr_t pc, uintptr_t sp, uintptr_t arg, uintptr_t second)
{
    k_memset(f, 0, sizeof(*f)); f->pc = pc & ~1u; f->status = 1u << 24; f->sp = sp; f->r[0] = arg; f->r[1] = second;
}
struct arch_frame *arch_return_frame(struct arch_frame *f)
{
    struct arch_frame *stack = (void *)(f->sp - offsetof(struct arch_frame, sp));
    k_memcpy(stack, f, offsetof(struct arch_frame, sp));
    return stack;
}
#else
struct tables { uint32_t root[4096], user[NOS_USER_PT_SLOTS][256], helper[256];
    unsigned short binding[NOS_USER_PT_SLOTS], live[NOS_USER_PT_SLOTS];
} __attribute__((aligned(16384)));
/* The stride must retain the 16 KiB TTBR alignment for every address space. */
static struct tables spaces[NOS_SPACES] __attribute__((aligned(16384)));
static int enabled[NOS_CPUS];
static unsigned kernel_tables_ready;
static uint32_t kernel_pages[128][256] __attribute__((aligned(1024)));
extern char __kernel_start[], __kernel_ro_end[], kernel_stack_guard[];
static unsigned period, claimed[NOS_CPUS];
void arch_platform_init(void) { }
void arch_putc(char c)
{ while (*reg(0x09000018) & 0x20) { } *reg(0x09000000) = (unsigned char)c; }
uintptr_t arch_irq_save(void)
{
    uintptr_t v; __asm__ volatile("mrs %0, cpsr; cpsid i" : "=r"(v) :: "memory"); return v;
}
void arch_irq_restore(uintptr_t v) { if (!(v & 0x80)) __asm__ volatile("cpsie i" ::: "memory"); }
static void rearm(void)
{
    unsigned ctl = 1;
    __asm__ volatile("mcr p15, 0, %0, c14, c3, 0; mcr p15, 0, %1, c14, c3, 1; isb" :: "r"(period), "r"(ctl));
}
void arch_timer_init(void)
{
    unsigned freq;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(freq));
    period = freq / 100;
    *reg(0x08000000) = 1;
    *reg(0x08000100) = 1u << 27;
    *reg(0x08000418) = 0x80808080;
    *reg(0x08010004) = 0xff;
    *reg(0x08010000) = 1;
    rearm();
}
void arch_timer_ack(void)
{ rearm(); *reg(0x08010010) = claimed[arch_cpu_id()]; }
void arch_space_init(unsigned id)
{
    struct tables *t = &spaces[id]; k_memset(t, 0, sizeof(*t));
    for (unsigned n = 0; n < NOS_USER_PT_SLOTS; n++) t->binding[n] = 0xffff;
    for (unsigned i = 0x400; i < 0x480; i++) t->root[i] = (i << 20) | 2 | (1u << 10) | (1u << 4) | (1u << 12) | (1u << 16);
    for (unsigned block = 0; block < 128; block++) {
        t->root[0x400 + block] = (uintptr_t)kernel_pages[block] | 1;
        if (!kernel_tables_ready) for (unsigned page = 0; page < 256; page++) {
            uintptr_t pa = 0x40000000u + block * 0x100000 + page * 4096;
            unsigned bits = 2 | (1u << 4) | (1u << 6) | (1u << 10);
            if (pa >= (uintptr_t)__kernel_start && pa < (uintptr_t)__kernel_ro_end) bits |= 1u << 9;
            else bits |= 1;
            kernel_pages[block][page] = (pa >= (uintptr_t)kernel_stack_guard && pa - (uintptr_t)kernel_stack_guard < NOS_SMP * 32768u &&
                ((pa - (uintptr_t)kernel_stack_guard) & 32767) < 4096) ? 0 : pa | bits;
        }
    }
    kernel_tables_ready = 1;
    t->root[0x80] = 0x08000000 | 2 | (1u << 10) | (1u << 4);
    t->root[0xa0] = 0x0a000000 | 2 | (1u << 10) | (1u << 4);
    t->root[0x90] = 0x09000000 | 2 | (1u << 10) | (1u << 4);
#ifdef NOS_LINUX_COMPAT
    extern char __kuser_start[];
    t->root[0xfff] = (uintptr_t)t->helper | 1;
    t->helper[0xf0] = (uintptr_t)__kuser_start | 2 | (1u << 6) | (1u << 10) | 0x220;
#endif

}
static uint32_t *user_pte(struct tables *t, uintptr_t va, int create, unsigned *slot)
{
    if (va < VM_USER_BASE || va >= VM_USER_END) return NULL;
    unsigned block = (unsigned)(va >> 20), free_slot = NOS_USER_PT_SLOTS;
    if ((t->root[block] & 3) == 2) return NULL;
    for (unsigned n = 0; n < NOS_USER_PT_SLOTS; n++) {
        if (t->binding[n] == block) { if (slot) *slot = n; return &t->user[n][(va >> 12) & 255]; }
        if (t->binding[n] == 0xffff && free_slot == NOS_USER_PT_SLOTS) free_slot = n;
    }
    if (!create || free_slot == NOS_USER_PT_SLOTS) return NULL;
    k_memset(t->user[free_slot], 0, sizeof(t->user[free_slot]));
    t->binding[free_slot] = (unsigned short)block;
    t->root[block] = (uintptr_t)t->user[free_slot] | 1;
    if (slot) *slot = free_slot;
    return &t->user[free_slot][(va >> 12) & 255];
}
static void retire_user_tables(int finish)
{
    for (unsigned id = 0; id < NOS_SPACES; id++) for (unsigned n = 0; n < NOS_USER_PT_SLOTS; n++) {
        struct tables *t = &spaces[id];
        if (t->binding[n] == 0xffff || t->live[n]) continue;
        if (!finish) t->root[t->binding[n]] = 0; else t->binding[n] = 0xffff;
    }
}
int arch_map(unsigned id, uintptr_t va, uintptr_t pa, unsigned flags)
{
    struct tables *t = &spaces[id];
    if ((va & 4095) || (pa & 4095) || ((flags & 6) == 6)) return -NOS_EINVAL;
    unsigned slot = 0; uint32_t *p = user_pte(t, va, !!flags, &slot);
    if (!p) return flags ? -NOS_ENOMEM : 0;
    if (!*p && flags) t->live[slot]++; else if (*p && !flags) t->live[slot]--;
    unsigned bits = 2 | (1u << 6) | (1u << 10);
    bits |= (flags & ARCH_MAP_PRIVILEGED) ? ((flags & NOS_PROT_WRITE) ? 0x10 : 0x210) :
        ((flags & NOS_PROT_WRITE) ? 0x30 : 0x220);
    if (!(flags & NOS_PROT_EXEC)) bits |= 1;
    *p = flags ? pa | bits : 0;
    return 0;
}
void arch_unmap(unsigned id, uintptr_t va) { (void)arch_map(id, va, 0, 0); }
void arch_tlb_sync(void)
{ retire_user_tables(0); unsigned zero = 0; __asm__ volatile("dsb ishst; mcr p15, 0, %0, c8, c3, 0; dsb ish; isb" :: "r"(zero) : "memory"); retire_user_tables(1); }
void arch_cache_sync(void)
{ unsigned zero = 0; __asm__ volatile("dsb ish; mcr p15, 0, %0, c7, c1, 0; dsb ish; isb" :: "r"(zero) : "memory"); }
unsigned arch_cpu_id(void)
{ unsigned id; __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(id)); return id & 255; }
unsigned arch_cpu_count(void) { return NOS_SMP; }
extern void secondary_start(void);
void arch_start_secondary(void)
{
    for (unsigned cpu = 1; cpu < NOS_SMP; cpu++) {
        register uintptr_t r0 __asm__("r0") = 0x84000003u;
        register uintptr_t r1 __asm__("r1") = cpu;
        register uintptr_t r2 __asm__("r2") = (uintptr_t)secondary_start;
        register uintptr_t r3 __asm__("r3") = 0;
        __asm__ volatile("hvc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "memory", "cc");
        if ((intptr_t)r0) kernel_fail("PSCI CPU_ON failed");
    }
}
void arch_secondary_init(void) { }
void arch_space_switch(unsigned id)
{
    uintptr_t ttbr = (uintptr_t)spaces[id].root;
    unsigned zero = 0, domain = 1;
    __asm__ volatile("dsb sy; mcr p15, 0, %0, c2, c0, 0; mcr p15, 0, %1, c8, c7, 0; dsb sy; isb" :: "r"(ttbr), "r"(zero) : "memory");
    if (!enabled[arch_cpu_id()]) {
        unsigned ctl;
        __asm__ volatile("mcr p15, 0, %0, c2, c0, 2; mcr p15, 0, %1, c3, c0, 0" :: "r"(zero), "r"(domain));
        __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(ctl));
        ctl = (ctl | 1 | (1u << 23)) & ~(1u << 29);
        __asm__ volatile("mcr p15, 0, %0, c1, c0, 0; isb" :: "r"(ctl) : "memory");
        enabled[arch_cpu_id()] = 1;
    }
}
void arch_frame_init(struct arch_frame *f, uintptr_t pc, uintptr_t sp, uintptr_t arg, uintptr_t second)
{ k_memset(f, 0, sizeof(*f)); f->pc = pc & ~(uintptr_t)1; f->sp = sp; f->status = 0x10 | ((pc & 1) ? 0x20 : 0); f->r[0] = arg; f->r[1] = second; }
struct arch_frame *arch_return_frame(struct arch_frame *f) { return f; }
#endif
void arch_shutdown(int failed)
{
#ifdef NOS_NK60_V2
    /* No debugger/semihosting dependency on physical hardware. Preserve the
     * failure UART message and halt with interrupts disabled. */
    (void)failed;
    __asm__ volatile("cpsid i" ::: "memory");
#else
    uintptr_t args[2] = { 0x20026, (uintptr_t)failed };
    register uintptr_t r0 __asm__("r0") = 0x20;
    register uintptr_t r1 __asm__("r1") = (uintptr_t)args;
#ifdef NOS_ARM_M
    __asm__ volatile("bkpt #0xab" : "+r"(r0) : "r"(r1) : "memory");
#else
    __asm__ volatile("svc #0x123456" : "+r"(r0) : "r"(r1) : "memory");
#endif
#endif
    for (;;) __asm__ volatile("wfi");
}

#ifdef NOS_NK60_V2
/* Only this fixed SPI2 route owns DMA1. General DMA registers are never
 * exposed through the MMIO capability. The synchronous operation keeps the
 * caller's validated source allocation alive through completion/quiescence. */
static void nk60_dma_stop(void)
{
    *reg(0x40026070u) &= ~1u; /* DMA1 stream4 EN */
    unsigned remaining = 10000u;
    while ((*reg(0x40026070u) & 1u) && --remaining) { }
    if (!remaining) {
        /* A stuck stream must not retain access to user memory on return. */
        *reg(0x40023810u) |= 1u << 21; /* AHB1RSTR: DMA1 reset */
        __asm__ volatile("dsb" ::: "memory");
        *reg(0x40023810u) &= ~(1u << 21);
        if (*reg(0x40026070u) & 1u) kernel_fail("NK60 DMA would not quiesce");
    }
    __asm__ volatile("dsb" ::: "memory");
    *reg(0x4002600cu) = 0x3du; /* HIFCR: all stream4 flags */
}
static intptr_t nk60_rgb_dma(struct task *task, uintptr_t source, size_t length)
{
    if (length != NOS_NK60_RGB_BYTES) return -NOS_EINVAL;
    if (!nk60_dma_sram_range(source, length) ||
        !user_access(task, source, length, 0) ||
        task_user_phys_read(task, source) != source ||
        task_user_phys_read(task, source + length - 1u) != source + length - 1u)
        return -NOS_EFAULT;
    *reg(0x40023830u) |= 1u << 21; /* DMA1 clock */
    (void)*reg(0x40023830u);
    nk60_dma_stop();
    if (!(*reg(0x40003800u) & (1u << 6)) ||
        !(*reg(0x40003804u) & (1u << 1))) return -NOS_EIO;
    *reg(0x40026074u) = (uint32_t)length;
    *reg(0x40026078u) = 0x4000380cu; /* fixed peripheral destination */
    *reg(0x4002607cu) = (uint32_t)source;
    *reg(0x40026084u) = 0; /* byte direct mode, FIFO disabled */
    *reg(0x40026070u) = (1u << 6) | (1u << 10) | (2u << 16);
    __asm__ volatile("dsb" ::: "memory");
    *reg(0x40026070u) |= 1u;
    uint32_t start = *reg(0x40000024u);
    unsigned remaining = 2000000u;
    intptr_t result = -NOS_ETIMEDOUT;
    while (--remaining && (uint32_t)(*reg(0x40000024u) - start) < 10000u) {
        uint32_t status = *reg(0x40026004u);
        if (status & 0x0du) { result = -NOS_EIO; break; }
        if (status & (1u << 5)) {
            result = *reg(0x40026074u) ? -NOS_EIO : 0;
            break;
        }
    }
    nk60_dma_stop();
    if (result) *reg(0x40003804u) &= ~(1u << 1); /* stop SPI DMA requests */
    return result;
}
#endif

intptr_t arch_device_operation(struct task *task, uintptr_t op, uintptr_t b, uintptr_t c)
{
#ifdef NOS_ARM_M
#ifdef NOS_NK60_V2
    if (op >= NOS_NK60_READ32 && op <= NOS_NK60_WRITE8) {
        unsigned width = op >= NOS_NK60_READ8 ? 1u : 4u;
        int write = op == NOS_NK60_WRITE32 || op == NOS_NK60_WRITE8;
        if (!nk60_mmio_allowed(b, width, write)) return -NOS_EACCES;
        if (width == 1u) {
            volatile uint8_t *byte = (volatile uint8_t *)b;
            if (write) { if (c > UINT8_MAX) return -NOS_EINVAL; *byte = (uint8_t)c; return 0; }
            return *byte;
        }
        if (write) { *reg(b) = (uint32_t)c; return 0; }
        return (intptr_t)*reg(b);
    }
    if (op == NOS_NK60_RGB_DMA) return nk60_rgb_dma(task, b, c);
#else
    (void)task;
#endif
    (void)b; (void)c;
    if (op == 5) { unsigned status = *reg(0x40011000); return ((status & (1u << 5)) ? 1 : 0) | ((status & (1u << 7)) ? 4 : 0); }
    if (op == 4) { if (b) *reg(0x4001100c) |= 1u << 5; else *reg(0x4001100c) &= ~(1u << 5); return 0; }
    if (!op) return (*reg(0x40011000) & (1u << 5)) ? (intptr_t)(*reg(0x40011004) & 255) : -NOS_EAGAIN;
    return -NOS_ENOTSUP;
#else
    if (op == 5) { unsigned status = *reg(0x09000018); return (!(status & 0x10) ? 1 : 0) | (!(status & 0x20) ? 4 : 0); }
    if (op == 4) { *reg(0x09000038) = b ? ((1u << 4) | (1u << 6)) : 0; return 0; }
    if (!op) {
        if (*reg(0x09000018) & 0x10) { *reg(0x09000044) = 1u << 6; return -NOS_EAGAIN; }
        return (intptr_t)(*reg(0x09000000) & 255);
    }
    if (op == 1) return b < 32 ? (intptr_t)(0x0a000000u + b * 0x200u) : -NOS_EINVAL;
    if (op == 2) {
        if (b < 0x0a000000u || b >= 0x0a004000u || (b & 4095) ||
            c < 0x03000000u || c >= 0x03100000u || (c & 4095)) return -NOS_EINVAL;
        struct tables *t = &spaces[task->space_id]; unsigned slot;
        uint32_t *pte = user_pte(t, c, 1, &slot);
        if (!pte) return -NOS_ENOMEM;
        if (!*pte) t->live[slot]++;
        *pte = b | 2 | 0x30 | 1;
        arch_tlb_sync(); return (intptr_t)c;
    }
    if (op == 3) { uintptr_t pa = task_user_phys(task, b); return pa ? (intptr_t)pa : -NOS_EFAULT; }
    return -NOS_ENOTSUP;
#endif
}

uintptr_t arch_user_helper_phys(uintptr_t va)
{
#if defined(NOS_LINUX_COMPAT) && !defined(NOS_ARM_M)
    extern char __kuser_start[];
    if (va >= 0xffff0000u && va < 0xffff1000u) return (uintptr_t)__kuser_start + va - 0xffff0000u;
#else
    (void)va;
#endif
    return 0;
}

/* PL031 data register reports UTC seconds.  Its page is privileged in
 * the existing UART device block; user mapping APIs exclude that block. */
#ifndef NOS_ARM_M
unsigned arm_fault_kind[NOS_CPUS];
#endif
unsigned arch_fault_signal(void)
{
#ifdef NOS_ARM_M
    return (*reg(0xe000ed28u) & (1u << 16)) ? 4 : 11;
#else
    unsigned kind = arm_fault_kind[arch_cpu_id()], status;
    if (!kind) return 4; /* Undefined instruction. */
    if (kind == 1) __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(status));
    else __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(status));
    unsigned cause = (status & 15) | ((status >> 6) & 16);
    return cause == 1 ? 7 : 11;
#endif
}

unsigned arch_fault_access(void)
{
#ifdef NOS_ARM_M
    return 0;
#else
    unsigned kind = arm_fault_kind[arch_cpu_id()], status;
    if (kind == 1) __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(status));
    else if (kind == 2) __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(status));
    else return 0;
    unsigned cause = (status & 15) | ((status >> 6) & 16);
    if (cause != 5 && cause != 7) return 0; /* Translation faults only. */
    if (kind == 1) return NOS_PROT_EXEC;
    return status & (1u << 11) ? NOS_PROT_WRITE : NOS_PROT_READ;
#endif
}

uintptr_t arch_fault_address(void)
{
#ifdef NOS_ARM_M
    unsigned status = *reg(0xe000ed28u);
    if (status & (1u << 7)) return *reg(0xe000ed34u);
    if (status & (1u << 15)) return *reg(0xe000ed38u);
    return 0;
#else
    uintptr_t address;
    if (arm_fault_kind[arch_cpu_id()] == 1)
        __asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(address));
    else if (arm_fault_kind[arch_cpu_id()] == 2)
        __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(address));
    else address = 0;
    return address;
#endif
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
#ifdef NOS_ARM_M
    *reg(0x4001100c) &= ~(1u << 5); arch_irq_mask(37, 1);
#ifdef NOS_NK60_V2
    if (*reg(0x40023834u) & (1u << 7)) {
        *reg(0x50000018u) = 0; /* GINTMSK: stop USB interrupt delivery. */
        *reg(0x50000804u) |= 1u << 1; /* DCTL: soft disconnect. */
    }
    arch_irq_mask(67, 1);
    if (*reg(0x40023830u) & (1u << 21)) nk60_dma_stop();
#endif
    return;
#else
    *reg(0x09000038) = 0; arch_irq_mask(33, 1);
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
#ifdef NOS_ARM_M
    uintptr_t ipsr; __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
    return ipsr == 15 ? 0 : (unsigned)ipsr - 16;
#else
    unsigned irq = *reg(0x0801000c); claimed[arch_cpu_id()] = irq;
    return (irq & 1023) == 27 ? 0 : irq & 1023;
#endif
}
void arch_irq_complete(unsigned irq)
{
#ifdef NOS_ARM_M
    (void)irq;
#else
    (void)irq; *reg(0x08010010) = claimed[arch_cpu_id()];
#endif
}
void arch_irq_mask(unsigned irq, int masked)
{
#ifdef NOS_ARM_M
    if (irq >= 82) return;
    *reg(0xe000e100u + (masked ? 0x80u : 0u) + (irq / 32) * 4) = 1u << (irq % 32);
#else
    if (irq >= 1020) return;
    *reg(0x08000000u + (masked ? 0x180u : 0x100u) + (irq / 32) * 4) = 1u << (irq % 32);
#endif
    __asm__ volatile("dsb sy" ::: "memory");
}
int arch_irq_bind(unsigned device_index)
{
#ifdef NOS_ARM_M
#ifdef NOS_NK60_V2
    if (device_index == NOS_NK60_USB_IRQ_DEVICE) { arch_irq_mask(67, 0); return 67; }
#endif
    if (device_index && device_index != 0x100) return -NOS_EINVAL;
    *reg(0x4001100c) |= 1u << 5; /* USART1 RXNE interrupt */
    arch_irq_mask(37, 0); return 37;
#else
    if (device_index >= 32 && device_index != 0x100) return -NOS_EINVAL;
    unsigned irq = device_index == 0x100 ? 33 : 48 + device_index;
    *(volatile unsigned char *)(uintptr_t)(0x08000800u + irq) = 1;
    *(volatile unsigned char *)(uintptr_t)(0x08000400u + irq) = 0x80;
    arch_irq_mask(irq, 0); return (int)irq;
#endif
}

unsigned arch_mapping_flags(unsigned id, uintptr_t va)
{
#ifdef NOS_ARM_M
    (void)id; (void)va; return 0;
#else
    if (id >= NOS_SPACES) return 0;
#ifdef NOS_LINUX_COMPAT
    if (va >= 0xffff0000u && va < 0xffff1000u) return spaces[id].helper[0xf0] ? 5 : 0;
#endif
    uint32_t *p = user_pte(&spaces[id], va, 0, NULL);
    if (!p || !*p) return 0;
    uint32_t entry = *p;
    return 1 | ((entry & 0x30) == 0x30 ? 2 : 0) | (!(entry & 1) ? 4 : 0);
#endif
}
void arch_fault_probe(void)
{ __asm__ volatile("udf #0" ::: "memory"); }
