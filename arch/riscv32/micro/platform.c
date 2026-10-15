/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
#ifdef NOS_MMU
struct tables { uint32_t root[1024], text[1024], data[1024], heap[1024], device[1024]; };
static struct tables spaces[NOS_TASKS] __attribute__((aligned(4096)));
static uint32_t kernel_pages[2][1024] __attribute__((aligned(4096)));
static unsigned kernel_tables_ready;
extern char __kernel_start[], __kernel_ro_end[], kernel_stack_guard[];
#endif
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
    uintptr_t value;
    __asm__ volatile("csrrc %0, sstatus, %1" : "=r"(value) : "r"((uintptr_t)2) : "memory");
    return value;
}
void arch_irq_restore(uintptr_t value)
{ if (value & 2) __asm__ volatile("csrs sstatus, %0" :: "r"((uintptr_t)2) : "memory"); }
void arch_timer_init(void) { __asm__ volatile("csrs sie, %0" :: "r"((uintptr_t)512)); }
void arch_timer_ack(void) { __asm__ volatile("csrc sip, %0" :: "r"((uintptr_t)2)); }
#ifdef NOS_MMU
static uint32_t table(void *pointer) { return (((uintptr_t)pointer >> 12) << 10) | 1; }
static uint32_t leaf(uintptr_t physical, unsigned flags, unsigned user)
{
    unsigned bits = 1 | 64 | 128;
    if (flags & NOS_PROT_READ) bits |= 2;
    if (flags & NOS_PROT_WRITE) bits |= 4;
    if (flags & NOS_PROT_EXEC) bits |= 8;
    if (user) bits |= 16;
    return ((physical >> 12) << 10) | bits;
}
#endif
void arch_space_init(unsigned id)
{
#ifdef NOS_MMU
    struct tables *t = &spaces[id]; k_memset(t, 0, sizeof(*t));
    if (!kernel_tables_ready) {
        for (unsigned block = 0; block < 2; block++) {
            for (unsigned page = 0; page < 1024; page++) {
                uintptr_t physical = 0x80000000u + block * 0x400000u + page * NOS_PAGE_SIZE;
                unsigned prot = physical >= (uintptr_t)__kernel_start && physical < (uintptr_t)__kernel_ro_end ? 5 : 3;
                unsigned guard = physical >= (uintptr_t)kernel_stack_guard &&
                    physical - (uintptr_t)kernel_stack_guard < NOS_SMP * 32768u &&
                    ((physical - (uintptr_t)kernel_stack_guard) & 32767u) < NOS_PAGE_SIZE;
                kernel_pages[block][page] = guard ? 0 : leaf(physical, prot, 0);
            }
        }
        kernel_tables_ready = 1;
    }
    for (unsigned block = 0; block < 32; block++)
        t->root[512 + block] = leaf(0x80000000u + block * 0x400000u, 3, 0);
    t->root[512] = table(kernel_pages[0]); t->root[513] = table(kernel_pages[1]);
    t->root[0] = leaf(0, 3, 0); /* QEMU test finisher. */
    t->root[64] = leaf(0x10000000u, 3, 0); /* Supervisor UART and virtio MMIO. */
    t->root[48] = leaf(0x0c000000u, 3, 0); /* PLIC supervisor contexts. */
    t->root[16] = leaf(0x02000000u, 3, 0); /* CLINT supervisor alias. */
    t->root[1] = table(t->text); t->root[4] = table(t->data);
    t->root[8] = table(t->heap); t->root[12] = table(t->device);
#else
    (void)id;
#endif
}
int arch_map(unsigned id, uintptr_t virtual, uintptr_t physical, unsigned flags)
{
#ifdef NOS_MMU
    struct tables *t = &spaces[id];
    uint32_t *p = virtual >> 22 == 1 ? t->text : virtual >> 22 == 4 ? t->data :
                  virtual >> 22 == 8 ? t->heap : virtual >> 22 == 12 ? t->device : NULL;
    if (!p || (virtual & 4095) || (physical & 4095)) return -NOS_EINVAL;
    p[(virtual >> 12) & 1023] = flags ? leaf(physical, flags, !(flags & ARCH_MAP_PRIVILEGED)) : 0;
#else
    (void)id; (void)flags;
    if (virtual != physical) return -NOS_ENOTSUP;
#endif
    return 0;
}
void arch_unmap(unsigned id, uintptr_t virtual)
{
#ifdef NOS_MMU
    (void)arch_map(id, virtual, 0, 0);
#else
    (void)id; (void)virtual;
#endif
}
static void remote_sync(void)
{
    if (!secondary_release) return;
    for (unsigned cpu = 0; cpu < NOS_SMP; cpu++) {
        if (cpu == arch_cpu_id()) continue;
        tlb_ack[cpu] = 0; __asm__ volatile("fence rw, rw" ::: "memory");
#ifdef NOS_MMU
        *(volatile uint32_t *)(uintptr_t)(0x04000000u + cpu * 4) = 1;
#else
        *(volatile uint32_t *)(uintptr_t)(0x02000000u + cpu * 4) = 1;
#endif
        while (!tlb_ack[cpu]) __asm__ volatile("nop" ::: "memory");
        __asm__ volatile("fence rw, rw" ::: "memory");
    }
}
void arch_tlb_sync(void) { __asm__ volatile("sfence.vma" ::: "memory"); remote_sync(); }
void arch_cache_sync(void) { __asm__ volatile("fence.i" ::: "memory"); remote_sync(); }
unsigned arch_cpu_id(void)
{ uintptr_t id; __asm__ volatile("mv %0, tp" : "=r"(id)); return (unsigned)id; }
unsigned arch_cpu_count(void) { return NOS_SMP; }
void arch_start_secondary(void) { __asm__ volatile("fence rw, rw" ::: "memory"); secondary_release = 1; }
void arch_secondary_init(void) { }
void arch_space_switch(unsigned id)
{
#ifdef NOS_MMU
    uintptr_t satp = 0x80000000u | ((uintptr_t)spaces[id].root >> 12);
#else
    (void)id; uintptr_t satp = 0;
#endif
    __asm__ volatile("csrw satp, %0; sfence.vma" :: "r"(satp) : "memory");
}
void arch_frame_init(struct arch_frame *frame, uintptr_t pc, uintptr_t sp, uintptr_t arg, uintptr_t second)
{
    k_memset(frame, 0, sizeof(*frame)); frame->pc = pc; frame->x[2] = sp;
    frame->x[10] = arg; frame->x[11] = second; frame->status = (1u << 5) | (3u << 13);
}
struct arch_frame *arch_return_frame(struct arch_frame *frame) { return frame; }
intptr_t arch_device_operation(struct task *task, uintptr_t op, uintptr_t b, uintptr_t c)
{
    volatile unsigned char *uart = (void *)0x10000000;
    if (!op) return (uart[5] & 1) ? uart[0] : -NOS_EAGAIN;
    if (op == 4) {
        if (b > 1) return -NOS_EINVAL;
        uart[1] = b ? 1 : 0; return 0;
    }
    if (op == 1) return b < 8 ? (intptr_t)(0x10001000u + b * 0x1000u) : -NOS_EINVAL;
    if (op == 3) { uintptr_t physical = task_user_phys(task, b); return physical ? (intptr_t)physical : -NOS_EFAULT; }
#ifdef NOS_MMU
    if (op == 2) {
        if (b < 0x10001000u || b >= 0x10009000u || (b & 4095) ||
            c < 0x03000000u || c >= 0x03200000u || (c & 4095)) return -NOS_EINVAL;
        spaces[task->space_id].device[(c >> 12) & 1023] = leaf(b, 3, 1);
        arch_tlb_sync(); return (intptr_t)c;
    }
#else
    if (op == 2) {
        if (b < 0x10001000u || b >= 0x10009000u || (b & 4095)) return -NOS_EINVAL;
        return (intptr_t)b;
    }
    (void)c;
#endif
    return -NOS_ENOTSUP;
}
void arch_device_revoke(struct task *task)
{
    if (!(task->caps & NOS_CAP_DEVICE)) return;
    ((volatile unsigned char *)0x10000000u)[1] = 0;
    arch_irq_mask(10, 1);
    for (unsigned i = 0; i < 8; i++) {
        volatile uint32_t *mmio = (void *)(uintptr_t)(0x10001000u + i * 0x1000u);
        if (mmio[0] != 0x74726976 || mmio[2] != 2) continue;
        mmio[0x70 / 4] = 0; __asm__ volatile("fence iorw, iorw" ::: "memory");
        unsigned remaining = 100000;
        while (mmio[0x70 / 4] && remaining) remaining--;
        if (!remaining) kernel_fail("device reset did not quiesce DMA");
    }
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
}
int arch_irq_bind(unsigned device_index)
{
    if (device_index >= 8 && device_index != 0x100) return -NOS_EINVAL;
    unsigned irq = device_index == 0x100 ? 10 : device_index + 1;
    *(volatile uint32_t *)(uintptr_t)(0x0c000000u + irq * 4) = 1;
    *(volatile uint32_t *)(uintptr_t)0x0c201000u = 0;
    arch_irq_mask(irq, 0); return (int)irq;
}
unsigned arch_mapping_flags(unsigned id, uintptr_t virtual)
{
#ifdef NOS_MMU
    if (id >= NOS_TASKS) return 0;
    struct tables *t = &spaces[id];
    uint32_t entry = t->root[virtual >> 22];
    if (!(entry & 1)) return 0;
    if (!(entry & 14)) {
        uint32_t *second = (uint32_t *)(uintptr_t)((entry >> 10) << 12);
        entry = second[(virtual >> 12) & 1023];
    }
    return entry & 1 ? (entry >> 1) & 7 : 0;
#else
    (void)id; (void)virtual;
    return NOS_PROT_READ | NOS_PROT_WRITE | NOS_PROT_EXEC; /* Flat PMP grants. */
#endif
}
void arch_fault_probe(void) { __asm__ volatile(".word 0" ::: "memory"); }
