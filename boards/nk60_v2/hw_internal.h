/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NK60_V2_HW_INTERNAL_H
#define NOS_NK60_V2_HW_INTERNAL_H

#include "hardware.h"

#define HW_EIO 5
#define HW_ENODEV 19
#define HW_EINVAL 22
#define HW_ENOSYS 38
#define HW_ETIMEDOUT 110

#define GPIO_MODER 0x00u
#define GPIO_OTYPER 0x04u
#define GPIO_OSPEEDR 0x08u
#define GPIO_PUPDR 0x0cu
#define GPIO_IDR 0x10u
#define GPIO_BSRR 0x18u
#define GPIO_AFRL 0x20u
#define RCC_AHB1ENR 0x30u
#define RCC_APB1ENR 0x40u
#define RCC_APB2ENR 0x44u
#define SPI_CR1 0x00u
#define SPI_CR2 0x04u
#define SPI_SR 0x08u
#define SPI_RXNE (1u << 0)
#define SPI_TXE (1u << 1)
#define SPI_BSY (1u << 7)
#define SPI_SPE (1u << 6)

static inline uint32_t hw_read(struct nk60_hw *hw, uintptr_t address)
{
    return hw->io.read32(hw->io.context, address);
}

static inline void hw_write(struct nk60_hw *hw, uintptr_t address, uint32_t value)
{
    hw->io.write32(hw->io.context, address, value);
}

static inline void hw_update(struct nk60_hw *hw, uintptr_t address,
                             uint32_t mask, uint32_t value)
{
    hw_write(hw, address, (hw_read(hw, address) & ~mask) | value);
}

static inline void hw_delay(struct nk60_hw *hw, uint32_t us)
{
    hw->io.delay_us(hw->io.context, us);
}

static inline uint32_t hw_time(struct nk60_hw *hw)
{
    return hw->io.time_us(hw->io.context);
}

static inline int hw_valid(const struct nk60_hw *hw)
{
    return hw && hw->io.read32 && hw->io.write32 &&
           hw->io.time_us && hw->io.delay_us;
}

static inline void hw_gpio_level(struct nk60_hw *hw, uintptr_t port,
                                 unsigned int pin, int level)
{
    hw_write(hw, port + GPIO_BSRR, 1u << (pin + (level ? 0u : 16u)));
}

static inline void hw_gpio_config(struct nk60_hw *hw, uintptr_t port,
                                  unsigned int pin, unsigned int mode,
                                  unsigned int pull, unsigned int alternate)
{
    unsigned int shift = pin * 2u;

    hw_update(hw, port + GPIO_OTYPER, 1u << pin, 0);
    hw_update(hw, port + GPIO_OSPEEDR, 3u << shift, 3u << shift);
    hw_update(hw, port + GPIO_PUPDR, 3u << shift, pull << shift);
    if (mode == 2u) {
        unsigned int af_shift = (pin & 7u) * 4u;
        uintptr_t af_reg = port + GPIO_AFRL + (pin / 8u) * 4u;

        hw_update(hw, af_reg, 15u << af_shift, alternate << af_shift);
    }
    hw_update(hw, port + GPIO_MODER, 3u << shift, mode << shift);
}

static inline int hw_wait(struct nk60_hw *hw, uintptr_t address,
                           uint32_t mask, uint32_t expected, uint32_t timeout_us)
{
    uint32_t start = hw_time(hw);
    unsigned int iterations;

    for (iterations = 0; iterations < 100000u; iterations++) {
        if ((hw_read(hw, address) & mask) == expected)
            return 0;
        if ((uint32_t)(hw_time(hw) - start) >= timeout_us)
            return -HW_ETIMEDOUT;
    }
    return -HW_ETIMEDOUT;
}

#endif
