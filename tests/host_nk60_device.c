/* Copyright (C) 2026 NOS contributors. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "nk60_device.h"

int main(void)
{
    const uintptr_t read_write[] = {
        0x40020000u, 0x40020424u, 0x40020820u, 0x40020014u,
        0x40023830u, 0x40023834u, 0x40023840u, 0x40023844u,
        0x40013000u, 0x40013004u, 0x4001300cu,
        0x40003800u, 0x40003804u, 0x4000380cu,
        0x50000000u, 0x50000104u, 0x50000800u, 0x5000083cu,
        0x50000900u, 0x5000093cu, 0x50000b00u, 0x50000b1cu,
        0x50000e00u, 0x50001000u, 0x50002000u, 0x50003000u,
        0x50000108u, 0x50000940u, 0x5000095cu, 0x50000b40u, 0x50000b5cu
    };
    for (unsigned i = 0; i < sizeof(read_write) / sizeof(read_write[0]); i++) {
        uintptr_t address = read_write[i];
        assert(nk60_mmio_allowed(address, 4, 0));
        assert(nk60_mmio_allowed(address, 4, 1));
        for (unsigned j = 1; j < 4; j++) {
            assert(!nk60_mmio_allowed(address + j, 4, 0));
            assert(!nk60_mmio_allowed(address + j, 4, 1));
        }
        assert(!nk60_mmio_allowed(address, 2, 0));
        assert(!nk60_mmio_allowed(address, 8, 1));
    }
    assert(nk60_mmio_allowed(0x4001300cu, 1, 0));
    assert(nk60_mmio_allowed(0x4001300cu, 1, 1));
    assert(!nk60_mmio_allowed(0x4001300du, 1, 1));
    assert(!nk60_mmio_allowed(0x50001000u, 1, 0));
    assert(nk60_mmio_allowed(0x40000024u, 4, 0));
    assert(!nk60_mmio_allowed(0x40000024u, 4, 1));
    for (unsigned i = 0; i < 3; i++) {
        uintptr_t base = (uintptr_t)0x40020000u + i * 0x400u;
        assert(nk60_mmio_allowed(base + 0x10u, 4, 0));
        assert(!nk60_mmio_allowed(base + 0x10u, 4, 1));
        assert(!nk60_mmio_allowed(base + 0x18u, 4, 0));
        assert(nk60_mmio_allowed(base + 0x18u, 4, 1));
        assert(!nk60_mmio_allowed(base + 0x1cu, 4, 0));
    }
    for (unsigned i = 0; i < 3; ++i) {
        assert(nk60_mmio_allowed(0x1fff7a10u + i * 4u, 4, 0));
        assert(!nk60_mmio_allowed(0x1fff7a10u + i * 4u, 4, 1));
    }
    assert(!nk60_mmio_allowed(0x1fff7a0cu, 4, 0));
    assert(!nk60_mmio_allowed(0x1fff7a1cu, 4, 0));
    puts("PASS nk60_mmio_valid_registers_and_widths");

    const uintptr_t forbidden[] = {
        0, UINTPTR_MAX, UINTPTR_MAX - 3u, 0x08000000u, 0x20000000u,
        0x40000000u, 0x40000028u, 0x40020c00u, 0x40023800u,
        0x40023804u, 0x40023808u, 0x40026000u, 0x40026070u,
        0x4002607cu, 0x40023810u, 0x5000010cu, 0x500007fcu,
        0x50000840u, 0x50000960u, 0x50000afcu, 0x50000b20u,
        0x50000dfcu, 0x50001004u, 0x50001ffcu, 0x50002004u, 0x50003004u, 0x50000b60u,
        0xe000e100u, 0xe000ed08u, 0xe000ed9cu
    };
    for (unsigned i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        assert(!nk60_mmio_allowed(forbidden[i], 4, 0));
        assert(!nk60_mmio_allowed(forbidden[i], 4, 1));
        assert(!nk60_mmio_allowed(forbidden[i], 1, 0));
        assert(!nk60_mmio_allowed(forbidden[i], 1, 1));
    }
#if UINTPTR_MAX > UINT32_MAX
    for (unsigned i = 0; i < sizeof(read_write) / sizeof(read_write[0]); i++) {
        uintptr_t alias = read_write[i] + ((uintptr_t)1u << 32);
        assert(!nk60_mmio_allowed(alias, 4, 0));
        assert(!nk60_mmio_allowed(alias, 4, 1));
    }
#endif
    puts("PASS nk60_mmio_dma_core_registers_and_aliases_denied");

    assert(nk60_dma_sram_range(0x20000000u, NOS_NK60_RGB_BYTES));
    uintptr_t last = 0x20010000u - NOS_NK60_RGB_BYTES;
    assert(nk60_dma_sram_range(last, NOS_NK60_RGB_BYTES));
    assert(!nk60_dma_sram_range(last + 1u, NOS_NK60_RGB_BYTES));
    assert(!nk60_dma_sram_range(0x1fffffffu, NOS_NK60_RGB_BYTES));
    assert(!nk60_dma_sram_range(0x20010000u, NOS_NK60_RGB_BYTES));
    assert(!nk60_dma_sram_range(0x08000000u, NOS_NK60_RGB_BYTES));
    assert(!nk60_dma_sram_range(UINTPTR_MAX - 10u, NOS_NK60_RGB_BYTES));
    assert(!nk60_dma_sram_range(0x20000000u, 0));
    assert(!nk60_dma_sram_range(0x20000000u, NOS_NK60_RGB_BYTES - 1u));
    assert(!nk60_dma_sram_range(0x20000000u, NOS_NK60_RGB_BYTES + 1u));
    assert(!nk60_dma_sram_range(0x20000000u, SIZE_MAX));
    puts("PASS nk60_dma_sram_edges_overflow_and_exact_length");
    return 0;
}
