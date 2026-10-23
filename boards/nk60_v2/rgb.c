/*
 * Copyright (C) 2023 胡启航<Nick Hu>, huqihan@live.com
 * Copyright (C) 2026 NOS contributors.
 * LED encoding and physical layout retained from the original nk60_v2 driver.
 */
#include "hw_internal.h"

const uint8_t nk60_canvas_to_led[NK60_CANVAS_WIDTH * NK60_CANVAS_HEIGHT] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 40,
    41, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 52,
    53, 54, 55, 56, 56, 56, 56, 56, 56, 56, 57, 58, 59, 60
};

static void rgb_line_low(struct nk60_hw *hw)
{
    hw_update(hw, NK60_SPI2 + SPI_CR2, 1u << 1, 0);
    hw_update(hw, NK60_SPI2 + SPI_CR1, SPI_SPE, 0);
    hw_gpio_level(hw, NK60_GPIO_C, 3, 0);
    hw_gpio_config(hw, NK60_GPIO_C, 3, 1, 0, 0);
    hw_delay(hw, 80);
}

int nk60_rgb_init(struct nk60_hw *hw)
{
    if (!hw_valid(hw))
        return -HW_EINVAL;
    hw->rgb_ready = 0;
    hw->rgb_powered = 0;
    hw_update(hw, NK60_RCC + RCC_AHB1ENR, 0, (1u << 1) | (1u << 2));
    hw_update(hw, NK60_RCC + RCC_APB1ENR, 0, 1u << 14);
    (void)hw_read(hw, NK60_RCC + RCC_APB1ENR);
    hw_gpio_level(hw, NK60_GPIO_B, 0, 0);
    hw_gpio_config(hw, NK60_GPIO_B, 0, 1, 0, 0);
    hw_write(hw, NK60_SPI2 + SPI_CR1, 0);
    hw_write(hw, NK60_SPI2 + SPI_CR2, 0);
    /* One-line TX, APB1=42 MHz /8, mode3, software NSS, MSB first. */
    hw_write(hw, NK60_SPI2 + SPI_CR1,
             (1u << 15) | (1u << 14) | (1u << 9) | (1u << 8) |
             (2u << 3) | (1u << 2) | 3u);
    rgb_line_low(hw);
    hw->rgb_ready = 1;
    return 0;
}

int nk60_rgb_set_power(struct nk60_hw *hw, int enabled)
{
    if (!hw_valid(hw))
        return -HW_EINVAL;
    if (!hw->rgb_ready)
        return -HW_ENODEV;
    rgb_line_low(hw);
    hw_gpio_level(hw, NK60_GPIO_B, 0, enabled != 0);
    hw->rgb_powered = enabled != 0;
    if (enabled)
        hw_delay(hw, 80);
    return 0;
}

int nk60_rgb_encode(const struct nk60_rgb *colors, size_t count,
                     uint8_t *wave, size_t wave_bytes)
{
    size_t led;

    if (!colors || !wave || count != NK60_LED_COUNT ||
        wave_bytes != NK60_RGB_WAVE_BYTES)
        return -HW_EINVAL;
    for (led = 0; led < count; led++) {
        uint8_t grb[3] = { colors[led].g, colors[led].r, colors[led].b };
        unsigned int component, bit;

        for (component = 0; component < 3; component++) {
            for (bit = 0; bit < 8; bit++)
                *wave++ = (grb[component] & (0x80u >> bit)) ? 0xf0 : 0xc0;
        }
    }
    return 0;
}

int nk60_rgb_write(struct nk60_hw *hw, const uint8_t *wave, size_t wave_bytes,
                    nk60_dma_tx_fn dma_tx, void *dma_context)
{
    size_t i;
    int error;

    if (!hw_valid(hw) || !wave || wave_bytes != NK60_RGB_WAVE_BYTES)
        return -HW_EINVAL;
    if (!hw->rgb_ready || !hw->rgb_powered)
        return -HW_ENODEV;
    if (!dma_tx)
        return -HW_ENOSYS;
    for (i = 0; i < wave_bytes; i++) {
        if (wave[i] != 0xc0 && wave[i] != 0xf0)
            return -HW_EINVAL;
    }
    hw_gpio_config(hw, NK60_GPIO_C, 3, 2, 0, 5);
    hw_update(hw, NK60_SPI2 + SPI_CR1, SPI_SPE, SPI_SPE);
    hw_update(hw, NK60_SPI2 + SPI_CR2, 1u << 1, 1u << 1);
    error = dma_tx(dma_context, NK60_SPI2 + NK60_SPI_DR, wave, wave_bytes, 10000);
    if (error > 0)
        error = -HW_EIO;
    if (!error)
        error = hw_wait(hw, NK60_SPI2 + SPI_SR, SPI_TXE, SPI_TXE, 1000);
    if (!error)
        error = hw_wait(hw, NK60_SPI2 + SPI_SR, SPI_BSY, 0, 1000);
    rgb_line_low(hw);
    if (error) {
        hw_gpio_level(hw, NK60_GPIO_B, 0, 0);
        hw->rgb_powered = 0;
    }
    return error;
}
