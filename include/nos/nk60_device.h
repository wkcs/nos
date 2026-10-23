/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NK60_DEVICE_H
#define NOS_NK60_DEVICE_H
#include <stddef.h>
#include <stdint.h>

/* NK60-only NOS_SYS_DEVICE operations. Other platforms return ENOTSUP. */
#define NOS_NK60_READ32 6u
#define NOS_NK60_WRITE32 7u
#define NOS_NK60_READ8 8u
#define NOS_NK60_WRITE8 9u
#define NOS_NK60_RGB_DMA 10u
#define NOS_NK60_RGB_BYTES (61u * 24u)
#define NOS_NK60_USB_IRQ_DEVICE 0x101u

/* This whitelist deliberately excludes DMA, NVIC, SCB and clock selection.
 * Device capability holders can configure only this board's peripherals.
 * DMA source/length validation is a separate fixed-destination operation. */
static inline int nk60_mmio_allowed(uintptr_t address, unsigned width, int write)
{
    if (width == 1u)
        return address == (uintptr_t)0x4001300cu; /* SPI1 DR byte transfer. */
    if (width != 4u || (address & 3u)) return 0;
    for (unsigned i = 0; i < 3u; i++) {
        uintptr_t base = (uintptr_t)0x40020000u + i * 0x400u;
        if (address < base || address > base + 0x24u) continue;
        unsigned offset = (unsigned)(address - base);
        if (offset <= 0x0cu || offset == 0x14u || offset == 0x20u || offset == 0x24u) return 1;
        return write ? offset == 0x18u : offset == 0x10u;
    }
    if (address == (uintptr_t)0x40023830u || address == (uintptr_t)0x40023834u ||
        address == (uintptr_t)0x40023840u ||
        address == (uintptr_t)0x40023844u) return 1; /* peripheral clock gates */
    if (!write && address == (uintptr_t)0x40000024u) return 1; /* TIM2 CNT, 1 MHz */
    if ((address >= (uintptr_t)0x40013000u && address <= (uintptr_t)0x4001300cu) ||
        (address >= (uintptr_t)0x40003800u && address <= (uintptr_t)0x4000380cu))
        return !write || (address & 0xfu) != 8u; /* SPI SR is read-only. */
    if (address < (uintptr_t)0x50000000u) return 0;
    uintptr_t offset = address - (uintptr_t)0x50000000u;
    return offset <= 0x104u || (offset >= 0x800u && offset <= 0x83cu) ||
        (offset >= 0x900u && offset <= 0x93cu) ||
        (offset >= 0xb00u && offset <= 0xb1cu) ||
        offset == 0xe00u || offset == 0x1000u || offset == 0x2000u;
}

/* Subtraction after a checked lower bound avoids end-address overflow. */
static inline int nk60_dma_sram_range(uintptr_t address, size_t bytes)
{
    return bytes == NOS_NK60_RGB_BYTES && address >= (uintptr_t)0x20000000u &&
        address - (uintptr_t)0x20000000u <= 0x10000u - NOS_NK60_RGB_BYTES;
}
#endif
