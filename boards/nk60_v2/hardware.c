/*
 * Copyright (C) 2023 胡启航<Nick Hu>, huqihan@live.com
 * Copyright (C) 2026 NOS contributors.
 * Wiring and scan order retained from the original nk60_v2 driver.
 */
#include "hw_internal.h"

const int8_t nk60_scan_to_key[NK60_SCAN_SLOTS] = {
     0,  1,  2,  3,  4,  5,  6,  7,
     8,  9, 10, 11, 12, 13, -1, -1,
    14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, -1, -1,
    36, 37, 38, 39, 40, -1, -1, -1,
    49, 50, 51, 52, -1, -1, -1, -1,
    53, 54, 55, 56, 57, 58, 59, 60,
    41, 42, 43, 44, 45, 46, 47, 48,
    28, 29, 30, 31, 32, 33, 34, 35
};

int nk60_hw_bind(struct nk60_hw *hw, const struct nk60_io *io)
{
    if (!hw || !io || !io->read32 || !io->write32 ||
        !io->time_us || !io->delay_us)
        return -HW_EINVAL;
    hw->io = *io;
    hw->progress = NULL;
    hw->progress_context = NULL;
    hw->flash_bytes = 0;
    hw->flash_ready = 0;
    hw->matrix_ready = 0;
    hw->rgb_ready = 0;
    hw->rgb_powered = 0;
    return 0;
}

void nk60_hw_set_progress(struct nk60_hw *hw, void (*progress)(void *), void *context)
{
    if (hw) {
        hw->progress = progress;
        hw->progress_context = context;
    }
}

static void shift_bit(struct nk60_hw *hw, int data)
{
    hw_gpio_level(hw, NK60_GPIO_B, 15, data);
    hw_delay(hw, 1);
    hw_gpio_level(hw, NK60_GPIO_B, 13, 1);
    hw_delay(hw, 1);
    hw_gpio_level(hw, NK60_GPIO_B, 13, 0);
}

static void latch(struct nk60_hw *hw)
{
    hw_gpio_level(hw, NK60_GPIO_B, 12, 1);
    hw_delay(hw, 1);
    hw_gpio_level(hw, NK60_GPIO_B, 12, 0);
    hw_delay(hw, 10);
}

int nk60_matrix_init(struct nk60_hw *hw)
{
    unsigned int i;

    if (!hw_valid(hw))
        return -HW_EINVAL;
    hw->matrix_ready = 0;
    hw_update(hw, NK60_RCC + RCC_AHB1ENR, 0, 3u);
    (void)hw_read(hw, NK60_RCC + RCC_AHB1ENR);
    hw_gpio_level(hw, NK60_GPIO_B, 12, 0);
    hw_gpio_level(hw, NK60_GPIO_B, 13, 0);
    hw_gpio_level(hw, NK60_GPIO_B, 15, 1);
    hw_gpio_config(hw, NK60_GPIO_B, 12, 1, 1, 0);
    hw_gpio_config(hw, NK60_GPIO_B, 13, 1, 1, 0);
    hw_gpio_config(hw, NK60_GPIO_B, 15, 1, 1, 0);
    hw_gpio_config(hw, NK60_GPIO_B, 14, 0, 1, 0);
    /* PA0 existed in the old input driver; its board function is unconfirmed. */
    hw_gpio_config(hw, NK60_GPIO_A, 0, 0, 1, 0);

    /* Deselect every key before the first one-low scan. */
    for (i = 0; i < NK60_SCAN_SLOTS; i++)
        shift_bit(hw, 1);
    latch(hw);
    hw->matrix_ready = 1;
    return 0;
}

int nk60_matrix_scan(struct nk60_hw *hw, uint64_t *pressed)
{
    uint64_t sample = 0;
    unsigned int slot;

    if (!hw_valid(hw) || !pressed)
        return -HW_EINVAL;
    if (!hw->matrix_ready)
        return -HW_ENODEV;
    for (slot = 0; slot < NK60_SCAN_SLOTS; slot++) {
        int key = nk60_scan_to_key[slot];

        shift_bit(hw, slot != 0);
        latch(hw);
        if (key >= 0 && !(hw_read(hw, NK60_GPIO_B + GPIO_IDR) & (1u << 14)))
            sample |= (uint64_t)1u << (unsigned int)key;
    }
    *pressed = sample;
    return 0;
}
