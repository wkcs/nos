/* Copyright (C) 2026 NOS contributors. */
#include "hw_internal.h"

static const struct nos_resource gpio_resources[] = {{NK60_GPIO_A, 0xc00, 0, NOS_RESOURCE_MMIO},
                                                     {NK60_RCC, 0x100, 0, NOS_RESOURCE_CLOCK}};
static const struct nos_resource spi_resources[] = {{NK60_SPI1, 0x10, 0, NOS_RESOURCE_MMIO},
                                                    {NK60_RCC, 0x100, 0, NOS_RESOURCE_CLOCK}};
static uintptr_t port_address(uint32_t pin)
{
    return NK60_GPIO_A + (pin / 16u) * 0x400u;
}
static int gpio_write(void *context, uint32_t pin, int level)
{
    struct nk60_hw *hw = context;
    hw->gpio_output[pin / 16u] = (uint16_t)((hw->gpio_output[pin / 16u] & ~(1u << (pin % 16u))) |
                                            ((unsigned)level << (pin % 16u)));
    hw_write(hw, port_address(pin) + GPIO_BSRR, 1u << ((pin % 16u) + (level ? 0 : 16)));
    return 0;
}
static int gpio_read(void *context, uint32_t pin, int *value)
{
    struct nk60_hw *hw = context;
    *value = (hw_read(hw, port_address(pin) + GPIO_IDR) >> (pin % 16u)) & 1u;
    return 0;
}
static int gpio_configure(void *context, uint32_t pin, const struct nos_gpio_config *c)
{
    struct nk60_hw *hw = context;
    uintptr_t port = port_address(pin);
    unsigned bit = pin % 16u, shift = bit * 2u;
    if (c->direction == NOS_GPIO_OUTPUT)
        gpio_write(hw, pin, c->initial_value);
    hw_update(hw, port + GPIO_OTYPER, 1u << bit, (uint32_t)c->open_drain << bit);
    hw_update(hw, port + GPIO_OSPEEDR, 3u << shift, 3u << shift);
    hw_update(hw, port + GPIO_PUPDR, 3u << shift, (uint32_t)c->pull << shift);
    hw_update(hw, port + GPIO_MODER, 3u << shift, (uint32_t)c->direction << shift);
    return 0;
}
static const struct nos_gpio_ops gpio_ops = {gpio_configure, gpio_read, gpio_write};
static int gpio_probe(struct nos_device *d)
{
    struct nk60_hw *hw = d->context;
    hw_update(hw, NK60_RCC + RCC_AHB1ENR, 0, 7u);
    (void)hw_read(hw, NK60_RCC + RCC_AHB1ENR);
    return 0;
}
static void gpio_remove(struct nos_device *d)
{
    struct nk60_hw *hw = d->context;
    hw->matrix_ready = hw->rgb_ready = hw->rgb_powered = 0;
    /* Board owns shared clock gates; do not disable another consumer's clock. */
    gpio_write(hw, 16, 0);
}
static const struct nos_driver gpio_driver = {"st,stm32f401-gpio", gpio_probe, gpio_remove, NULL,
                                              NULL};
static int spi_probe(struct nos_device *d)
{
    struct nk60_hw *hw = d->context;
    unsigned pin;
    if (!hw->io.read8 || !hw->io.write8)
        return -HW_EINVAL;
    hw_update(hw, NK60_RCC + RCC_APB2ENR, 0, 1u << 12);
    (void)hw_read(hw, NK60_RCC + RCC_APB2ENR);
    hw_gpio_level(hw, NK60_GPIO_B, 2, 1);
    hw_gpio_config(hw, NK60_GPIO_B, 2, 1, 1, 0);
    for (pin = 3; pin <= 5; pin++)
        hw_gpio_config(hw, NK60_GPIO_B, pin, 2, 1, 5);
    hw_write(hw, NK60_SPI1 + SPI_CR1, 0);
    hw_write(hw, NK60_SPI1 + SPI_CR2, 0);
    return 0;
}
static void spi_remove(struct nos_device *d)
{
    struct nk60_hw *hw = d->context;
    hw_gpio_level(hw, NK60_GPIO_B, 2, 1);
    hw_write(hw, NK60_SPI1 + SPI_CR1, 0);
    hw->nor.ready = hw->flash_ready = 0;
    hw->nor.capacity = hw->flash_bytes = 0;
}
static const struct nos_driver spi_driver = {"st,stm32f401-spi", spi_probe, spi_remove, NULL, NULL};
static int spi_message(void *context, const struct nos_spi_config *config,
                       const struct nos_spi_segment *segments, size_t count, uint32_t timeout)
{
    struct nk60_hw *hw = context;
    uint32_t start = hw_time(hw), rate = 84000000u;
    unsigned divider;
    size_t s, i;
    int error = 0;
    if (config->lsb_first)
        return -NOS_DRV_ENOTSUP;
    for (divider = 0; divider < 8 && (rate >> (divider + 1)) > config->hz; divider++)
        ;
    if (divider == 8)
        return -NOS_DRV_ENOTSUP;
    hw_write(hw, NK60_SPI1 + SPI_CR1,
             (1u << 9) | (1u << 8) | SPI_SPE | (divider << 3) | (1u << 2) | config->mode);
    hw_gpio_level(hw, NK60_GPIO_B, 2, 0);
    for (s = 0; !error && s < count; s++) {
        const uint8_t *tx = segments[s].tx;
        uint8_t *rx = segments[s].rx;
        for (i = 0; !error && i < segments[s].length; i++) {
            uint8_t value;
            uint32_t elapsed = (uint32_t)(hw_time(hw) - start);
            if (elapsed >= timeout) {
                error = -HW_ETIMEDOUT;
                break;
            }
            error = hw_wait(hw, NK60_SPI1 + SPI_SR, SPI_TXE, SPI_TXE, timeout - elapsed);
            if (error)
                break;
            hw->io.write8(hw->io.context, NK60_SPI1 + NK60_SPI_DR,
                          tx ? tx[i] : segments[s].tx_fill);
            elapsed = (uint32_t)(hw_time(hw) - start);
            if (elapsed >= timeout) {
                error = -HW_ETIMEDOUT;
                break;
            }
            error = hw_wait(hw, NK60_SPI1 + SPI_SR, SPI_RXNE, SPI_RXNE, timeout - elapsed);
            if (error)
                break;
            value = hw->io.read8(hw->io.context, NK60_SPI1 + NK60_SPI_DR);
            if (rx)
                rx[i] = value;
        }
    }
    if (!error) {
        uint32_t elapsed = (uint32_t)(hw_time(hw) - start);
        error = elapsed >= timeout ? -HW_ETIMEDOUT
                                   : hw_wait(hw, NK60_SPI1 + SPI_SR, SPI_BSY, 0, timeout - elapsed);
    }
    hw_gpio_level(hw, NK60_GPIO_B, 2, 1);
    if (error) {
        /* Abort the shift engine and clear RX/overrun before the next message. */
        hw_write(hw, NK60_SPI1 + SPI_CR1, 0);
        (void)hw->io.read8(hw->io.context, NK60_SPI1 + NK60_SPI_DR);
        (void)hw_read(hw, NK60_SPI1 + SPI_SR);
    }
    return error;
}
static const struct nos_spi_ops spi_ops = {spi_message};
static uint32_t nor_time(void *context)
{
    return hw_time(context);
}
static void nor_delay(void *context, uint32_t us)
{
    hw_delay(context, us);
}
static void nor_progress(void *context)
{
    struct nk60_hw *hw = context;
    if (hw->progress)
        hw->progress(hw->progress_context);
}
int nk60_controllers_init(struct nk60_hw *hw)
{
    struct nos_device *entries[2];
    struct nos_device_registry registry = {entries, 2, 0};
    int error;
    hw->gpio_output[0] = hw->gpio_output[1] = hw->gpio_output[2] = 0;
    hw->gpio_device = (struct nos_device){.name = "gpio",
                                          .compatible = "st,stm32f401-gpio",
                                          .context = hw,
                                          .resources = gpio_resources,
                                          .resource_count = 2};
    hw->spi_device = (struct nos_device){.name = "spi1",
                                         .compatible = "st,stm32f401-spi",
                                         .parent = &hw->gpio_device,
                                         .context = hw,
                                         .resources = spi_resources,
                                         .resource_count = 2};
    hw->gpio = (struct nos_gpio_controller){{&hw->gpio_device, hw, 1, 0}, &gpio_ops, 48};
    hw->spi = (struct nos_spi_bus){{&hw->spi_device, hw, 260, 0}, &spi_ops, 21000000, 1, 15};
    hw->nor = (struct nos_spi_nor){.spi = {&hw->spi, {21000000, 0, 3, 8, 0}},
                                   .context = hw,
                                   .time_us = nor_time,
                                   .delay_us = nor_delay,
                                   .progress = nor_progress};
    error = nos_device_register(&registry, &hw->gpio_device);
    if (!error)
        error = nos_device_register(&registry, &hw->spi_device);
    if (!error)
        error = nos_device_bind(&hw->gpio_device, &gpio_driver);
    /* This board has a closed static set; the temporary enumeration table is
     * discarded, device/controller objects remain owned by hw for its lifetime. */
    return error;
}
int nk60_spi_start(struct nk60_hw *hw)
{
    return hw->spi_device.state == NOS_DEVICE_ACTIVE
               ? 0
               : nos_device_bind(&hw->spi_device, &spi_driver);
}
