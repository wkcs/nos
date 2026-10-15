/* Copyright (C) 2026 NOS contributors */
#include "devices.h"
#ifndef NOS_ARM_M
#define QUEUE_SIZE 8u
struct virtq_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct virtq_avail { uint16_t flags, index, ring[QUEUE_SIZE], event; };
struct virtq_used_elem { uint32_t id, length; };
struct virtq_used { uint16_t flags, index; struct virtq_used_elem ring[QUEUE_SIZE]; uint16_t event; };
struct blk_request { uint32_t type, reserved; uint64_t sector; };
static volatile uint32_t *mmio;
static unsigned char *dma;
static uintptr_t dma_physical;
static unsigned queue_index, version, flush_supported, failed;
static uint64_t capacity, interrupts, poll_fallbacks;
static intptr_t irq_handle = -1;
static unsigned device_index;
static int pointer_error(intptr_t value)
{ return (uintptr_t)value >= (uintptr_t)-4095; }
static void barrier(void)
{
#if defined(NOS_RISCV) || defined(NOS_RISCV32)
    __asm__ volatile("fence iorw, iorw" ::: "memory");
#else
    __asm__ volatile("dmb sy" ::: "memory");
#endif
}
static uint32_t reg_read(unsigned offset) { return mmio[offset / 4]; }
static void reg_write(unsigned offset, uint32_t value) { mmio[offset / 4] = value; }
static void address_write(unsigned offset, uintptr_t address)
{ reg_write(offset, (uint32_t)address); reg_write(offset + 4, (uint32_t)((uint64_t)address >> 32)); }
int nos_block_init(void)
{
    if (mmio) return failed ? -NOS_EIO : 0;
    for (unsigned i = 0; i < 32; i++) {
        intptr_t pa = nos_syscall(NOS_SYS_DEVICE, 1, i, 0);
        if (pointer_error(pa)) break;
        uintptr_t va = 0x03000000u + i * NOS_PAGE_SIZE;
        intptr_t mapped = nos_syscall(NOS_SYS_DEVICE, 2, (uintptr_t)pa & ~(uintptr_t)(NOS_PAGE_SIZE - 1), va);
        if (pointer_error(mapped)) continue;
        volatile uint32_t *candidate = (volatile uint32_t *)((uintptr_t)mapped + ((uintptr_t)pa & (NOS_PAGE_SIZE - 1)));
        if (candidate[0] == 0x74726976u && candidate[2] == 2u) { mmio = candidate; device_index = i; break; }
    }
    if (!mmio) return -NOS_ENOENT;
    version = reg_read(4); if (version != 1 && version != 2) { mmio = 0; return -NOS_ENOTSUP; }
    reg_write(0x70, 0); reg_write(0x70, 1); reg_write(0x70, 3);
    reg_write(0x14, 0); unsigned features = reg_read(0x10);
    /* Read-only block devices are refused instead of silently dropping writes. */
    if (features & (1u << 5)) { mmio = 0; return -NOS_EACCES; }
    flush_supported = !!(features & (1u << 9));
    reg_write(0x24, 0); reg_write(0x20, features & (1u << 9));
    if (version == 2) {
        reg_write(0x14, 1); if (!(reg_read(0x10) & 1u)) { mmio = 0; return -NOS_ENOTSUP; }
        reg_write(0x24, 1); reg_write(0x20, 1); reg_write(0x70, 11);
        if (!(reg_read(0x70) & 8)) { mmio = 0; return -NOS_EIO; }
    }
    reg_write(0x30, 0); if (reg_read(0x34) < QUEUE_SIZE) { mmio = 0; return -NOS_ENOSPC; }
    intptr_t allocation = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, 3 * NOS_PAGE_SIZE, 0);
    if (pointer_error(allocation)) { mmio = 0; return (int)allocation; }
    dma = (unsigned char *)(uintptr_t)allocation;
    intptr_t physical = nos_syscall(NOS_SYS_DEVICE, 3, (uintptr_t)dma, 0);
    if (pointer_error(physical) || ((uintptr_t)physical & (NOS_PAGE_SIZE - 1))) {
        nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)dma, 0, 0); dma = 0; mmio = 0; return -NOS_EFAULT;
    }
    dma_physical = (uintptr_t)physical;
    for (unsigned i = 1; i < 3; i++) if ((uintptr_t)nos_syscall(NOS_SYS_DEVICE, 3, (uintptr_t)dma + i * NOS_PAGE_SIZE, 0) != dma_physical + i * NOS_PAGE_SIZE) {
        nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)dma, 0, 0); dma = 0; mmio = 0; return -NOS_ENOTSUP;
    }
    u_memset(dma, 0, 3 * NOS_PAGE_SIZE); reg_write(0x38, QUEUE_SIZE);
    if (version == 1) { reg_write(0x28, NOS_PAGE_SIZE); reg_write(0x3c, NOS_PAGE_SIZE); reg_write(0x40, (uint32_t)(dma_physical / NOS_PAGE_SIZE)); }
    else {
        address_write(0x80, dma_physical); address_write(0x90, dma_physical + QUEUE_SIZE * sizeof(struct virtq_desc));
        address_write(0xa0, dma_physical + NOS_PAGE_SIZE); reg_write(0x44, 1);
    }
    capacity = (uint64_t)reg_read(0x100) | ((uint64_t)reg_read(0x104) << 32);
    if (capacity < 66) { reg_write(0x70, 0); nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)dma, 0, 0); dma = 0; mmio = 0; return -NOS_ENOSPC; }
    irq_handle = nos_syscall(NOS_SYS_IRQ_BIND, device_index, 0, 0);
    if (irq_handle < 0) {
        int error = (int)irq_handle; reg_write(0x70, 0);
        nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)dma, 0, 0); dma = 0; mmio = 0; return error;
    }
    barrier(); reg_write(0x70, version == 2 ? 15 : 7); return 0;
}
static int transfer(unsigned type, unsigned sector, void *buffer)
{
    int rc = nos_block_init(); if (rc) return rc;
    if (type != 4 && sector >= capacity) return -NOS_EINVAL;
    if (type == 4 && !flush_supported) return -NOS_ENOTSUP;
    struct virtq_desc *desc = (void *)dma;
    volatile struct virtq_avail *avail = (void *)(dma + QUEUE_SIZE * sizeof(*desc));
    volatile struct virtq_used *used = (void *)(dma + NOS_PAGE_SIZE);
    struct blk_request *request = (void *)(dma + 2 * NOS_PAGE_SIZE);
    unsigned char *data = dma + 2 * NOS_PAGE_SIZE + 32;
    volatile unsigned char *status = dma + 2 * NOS_PAGE_SIZE + 544;
    request->type = type; request->reserved = 0; request->sector = sector; *status = 0xff;
    if (type == 1) u_memcpy(data, buffer, 512);
    desc[0] = (struct virtq_desc){ .address = dma_physical + 2 * NOS_PAGE_SIZE, .length = sizeof(*request), .flags = 1, .next = type == 4 ? 2 : 1 };
    desc[1] = (struct virtq_desc){ .address = dma_physical + 2 * NOS_PAGE_SIZE + 32, .length = 512, .flags = type == 0 ? 3 : 1, .next = 2 };
    desc[2] = (struct virtq_desc){ .address = dma_physical + 2 * NOS_PAGE_SIZE + 544, .length = 1, .flags = 2 };
    uint16_t previous = used->index;
    avail->ring[queue_index % QUEUE_SIZE] = 0; barrier(); avail->index = (uint16_t)++queue_index; barrier(); reg_write(0x50, 0);
    uintptr_t start = (uintptr_t)nos_syscall(NOS_SYS_TICKS, 0, 0, 0);
    for (;;) {
        /* The hardware IRQ object blocks this user service until notification;
         * neither a scheduler tick nor a completed DMA alone proves IRQ delivery. */
        intptr_t notices = nos_syscall(NOS_SYS_IRQ_WAIT, (uintptr_t)irq_handle, 100, 0);
        barrier();
        if (notices < 0) {
            if (notices == -NOS_ETIMEDOUT && used->index != previous) {
                poll_fallbacks++; break;
            }
            failed = 1; reg_write(0x70, reg_read(0x70) | 128); return (int)notices;
        }
        interrupts += (uintptr_t)notices;
        reg_write(0x64, reg_read(0x60)); barrier();
        intptr_t ack = nos_syscall(NOS_SYS_IRQ_ACK, (uintptr_t)irq_handle, 0, 0);
        if (ack < 0) { failed = 1; reg_write(0x70, reg_read(0x70) | 128); return (int)ack; }
        if (used->index != previous) break;
        if ((uintptr_t)nos_syscall(NOS_SYS_TICKS, 0, 0, 0) - start > 100) {
            failed = 1; reg_write(0x70, reg_read(0x70) | 128); return -NOS_ETIMEDOUT;
        }
    }
    if ((uint16_t)(used->index - previous) != 1 || used->ring[previous % QUEUE_SIZE].id != 0 ||
        used->ring[previous % QUEUE_SIZE].length < (type == 0 ? 513u : 1u) || *status) {
        failed = 1; reg_write(0x70, reg_read(0x70) | 128); return *status == 2 ? -NOS_ENOTSUP : -NOS_EIO;
    }
    if (type == 0) u_memcpy(buffer, data, 512);
    return 0;
}
int nos_block_read(unsigned sector, void *buffer) { return transfer(0, sector, buffer); }
int nos_block_write(unsigned sector, const void *buffer) { return transfer(1, sector, (void *)buffer); }
int nos_block_flush(void) { return transfer(4, 0, 0); }
uint64_t nos_block_capacity(void) { return capacity; }
uint64_t nos_block_interrupts(void) { return interrupts; }
uint64_t nos_block_requests(void) { return queue_index; }
uint64_t nos_block_poll_fallbacks(void) { return poll_fallbacks; }
unsigned nos_block_version(void) { return version; }
#else
int nos_block_init(void) { return -NOS_ENOTSUP; }
int nos_block_read(unsigned sector, void *buffer) { (void)sector; (void)buffer; return -NOS_ENOTSUP; }
int nos_block_write(unsigned sector, const void *buffer) { (void)sector; (void)buffer; return -NOS_ENOTSUP; }
int nos_block_flush(void) { return -NOS_ENOTSUP; }
uint64_t nos_block_capacity(void) { return 0; }
uint64_t nos_block_interrupts(void) { return 0; }
uint64_t nos_block_requests(void) { return 0; }
uint64_t nos_block_poll_fallbacks(void) { return 0; }
unsigned nos_block_version(void) { return 0; }
#endif
