/* Copyright (C) 2026 NOS contributors. Entropy comes only from virtio-rng. */
#include "devices.h"
#ifdef NOS_MMU
#define RNG_QUEUE 8u
struct rng_descriptor { uint64_t address; uint32_t length; uint16_t flags, next; };
struct rng_available { uint16_t flags, index, ring[RNG_QUEUE], event; };
struct rng_used_entry { uint32_t id, length; };
struct rng_used { uint16_t flags, index; struct rng_used_entry ring[RNG_QUEUE]; };
static volatile uint32_t *rng_mmio;
static unsigned char *rng_dma;
static uintptr_t rng_physical;
static unsigned rng_queue_index, rng_pending, rng_failed, rng_available_bytes, rng_consumed;
static uint16_t rng_previous;
static unsigned char rng_cache[64];
static void rng_barrier(void)
{
#if defined(NOS_RISCV)
    __asm__ volatile("fence iorw, iorw" ::: "memory");
#else
    __asm__ volatile("dmb sy" ::: "memory");
#endif
}
static uint32_t rng_get(unsigned offset) { return rng_mmio[offset / 4]; }
static void rng_set(unsigned offset, uint32_t value) { rng_mmio[offset / 4] = value; }
static void rng_address(unsigned offset, uintptr_t value)
{ rng_set(offset, (uint32_t)value); rng_set(offset + 4, (uint32_t)((uint64_t)value >> 32)); }
static int rng_initialize(void)
{
    if (rng_mmio) return rng_failed ? -NOS_EIO : 0;
    for (unsigned i = 0; i < 32; i++) {
        intptr_t physical = nos_syscall(NOS_SYS_DEVICE, 1, i, 0); if (nos_pointer_is_error(physical)) break;
        uintptr_t virtual = 0x03000000u + i * NOS_PAGE_SIZE;
        intptr_t mapped = nos_syscall(NOS_SYS_DEVICE, 2, (uintptr_t)physical & ~(uintptr_t)4095, virtual);
        if (nos_pointer_is_error(mapped)) continue;
        volatile uint32_t *candidate = (void *)((uintptr_t)mapped + ((uintptr_t)physical & 4095));
        if (candidate[0] == 0x74726976u && candidate[2] == 4) { rng_mmio = candidate; break; }
    }
    if (!rng_mmio) return -19;
    unsigned version = rng_get(4); if (version != 1 && version != 2) { rng_mmio = 0; return -NOS_ENOTSUP; }
    rng_set(0x70, 0); rng_set(0x70, 1); rng_set(0x70, 3);
    rng_set(0x24, 0); rng_set(0x20, 0);
    if (version == 2) {
        rng_set(0x14, 1); if (!(rng_get(0x10) & 1)) { rng_mmio = 0; return -NOS_ENOTSUP; }
        rng_set(0x24, 1); rng_set(0x20, 1); rng_set(0x70, 11);
        if (!(rng_get(0x70) & 8)) { rng_mmio = 0; return -NOS_EIO; }
    }
    rng_set(0x30, 0); if (rng_get(0x34) < RNG_QUEUE) { rng_mmio = 0; return -NOS_ENOSPC; }
    intptr_t allocated = nos_syscall(NOS_SYS_MMAP, 3, 3 * NOS_PAGE_SIZE, 0);
    if (nos_pointer_is_error(allocated)) { rng_mmio = 0; return (int)allocated; }
    rng_dma = (void *)(uintptr_t)allocated;
    intptr_t physical = nos_syscall(NOS_SYS_DEVICE, 3, (uintptr_t)rng_dma, 0);
    if (nos_pointer_is_error(physical)) goto bad_dma;
    rng_physical = (uintptr_t)physical;
    for (unsigned i = 0; i < 3; i++)
        if ((uintptr_t)nos_syscall(NOS_SYS_DEVICE, 3, (uintptr_t)rng_dma + i * NOS_PAGE_SIZE, 0) != rng_physical + i * NOS_PAGE_SIZE) goto bad_dma;
    u_memset(rng_dma, 0, 3 * NOS_PAGE_SIZE);
    /* This driver polls its queue; suppress queue IRQs rather than leaving an
     * unbound interrupt asserted. The entropy is still real device DMA. */
    ((struct rng_available *)(rng_dma + RNG_QUEUE * sizeof(struct rng_descriptor)))->flags = 1;
    rng_set(0x38, RNG_QUEUE);
    if (version == 1) { rng_set(0x28, NOS_PAGE_SIZE); rng_set(0x3c, NOS_PAGE_SIZE); rng_set(0x40, (uint32_t)(rng_physical / NOS_PAGE_SIZE)); }
    else { rng_address(0x80, rng_physical); rng_address(0x90, rng_physical + RNG_QUEUE * sizeof(struct rng_descriptor)); rng_address(0xa0, rng_physical + NOS_PAGE_SIZE); rng_set(0x44, 1); }
    rng_barrier(); rng_set(0x70, version == 2 ? 15 : 7); return 0;
bad_dma:
    rng_set(0x70, 0); nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)rng_dma, 0, 0); rng_dma = 0; rng_mmio = 0; return -NOS_EFAULT;
}
static void rng_submit(void)
{
    struct rng_descriptor *descriptors = (void *)rng_dma;
    volatile struct rng_available *available = (void *)(rng_dma + RNG_QUEUE * sizeof(*descriptors));
    volatile struct rng_used *used = (void *)(rng_dma + NOS_PAGE_SIZE);
    rng_previous = used->index;
    descriptors[0] = (struct rng_descriptor){ .address = rng_physical + 2 * NOS_PAGE_SIZE, .length = 64, .flags = 2 };
    available->ring[rng_queue_index % RNG_QUEUE] = 0; rng_barrier(); available->index = (uint16_t)++rng_queue_index;
    rng_pending = 1; rng_barrier(); rng_set(0x50, 0);
}
intptr_t nos_random_read(void *buffer, size_t count, unsigned flags)
{
    if (flags & ~3u) return -NOS_EINVAL; /* GRND_NONBLOCK and GRND_RANDOM. */
    if (!count) return 0;
    int rc = rng_initialize(); if (rc) return rc;
    if (rng_consumed == rng_available_bytes) {
        if (!rng_pending) rng_submit();
        volatile struct rng_used *used = (void *)(rng_dma + NOS_PAGE_SIZE);
        uintptr_t started = (uintptr_t)nos_syscall(NOS_SYS_TICKS, 0, 0, 0);
        for (;;) {
            rng_barrier();
            if (used->index != rng_previous) break;
            if (flags & 1u) return -NOS_EAGAIN;
            if ((uintptr_t)nos_syscall(NOS_SYS_TICKS, 0, 0, 0) - started > 100) { rng_failed = 1; rng_set(0x70, rng_get(0x70) | 128); return -NOS_ETIMEDOUT; }
            nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
        }
        rng_barrier();
        unsigned length = used->ring[rng_previous % RNG_QUEUE].length;
        if ((uint16_t)(used->index - rng_previous) != 1 || used->ring[rng_previous % RNG_QUEUE].id || !length || length > 64) {
            rng_failed = 1; rng_set(0x70, rng_get(0x70) | 128); return -NOS_EIO;
        }
        u_memcpy(rng_cache, rng_dma + 2 * NOS_PAGE_SIZE, length); rng_available_bytes = length; rng_consumed = 0; rng_pending = 0;
        rng_set(0x64, rng_get(0x60));
    }
    if (count > rng_available_bytes - rng_consumed) count = rng_available_bytes - rng_consumed;
    u_memcpy(buffer, rng_cache + rng_consumed, count);
    u_memset(rng_cache + rng_consumed, 0, count); rng_consumed += count; return (intptr_t)count;
}
#else
intptr_t nos_random_read(void *buffer, size_t count, unsigned flags)
{ (void)buffer; (void)count; (void)flags; return -19; }
#endif
