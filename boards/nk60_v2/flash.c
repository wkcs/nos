/*
 * Copyright (C) 2023 胡启航<Nick Hu>, huqihan@live.com
 * Copyright (C) 2026 NOS contributors.
 * SPI wiring retained from the original nk60_v2 W25Q driver.
 */
#include "hw_internal.h"

#define FLASH_TIMEOUT_US 1000000u
#define FLASH_ERASE_TIMEOUT_US 3000000u
#define FLASH_BUSY 1u
#define FLASH_WEL 2u

static void flash_progress(struct nk60_hw *hw)
{
    if (hw->progress)
        hw->progress(hw->progress_context);
}

static void flash_select(struct nk60_hw *hw, int selected)
{
    hw_gpio_level(hw, NK60_GPIO_B, 2, !selected);
}

static int spi_byte(struct nk60_hw *hw, uint8_t tx, uint8_t *rx)
{
    int error;
    uint8_t value;

    error = hw_wait(hw, NK60_SPI1 + SPI_SR, SPI_TXE, SPI_TXE, 1000);
    if (error)
        return error;
    hw->io.write8(hw->io.context, NK60_SPI1 + NK60_SPI_DR, tx);
    error = hw_wait(hw, NK60_SPI1 + SPI_SR, SPI_RXNE, SPI_RXNE, 1000);
    if (error)
        return error;
    value = hw->io.read8(hw->io.context, NK60_SPI1 + NK60_SPI_DR);
    if (rx)
        *rx = value;
    return 0;
}

static int flash_finish(struct nk60_hw *hw, int error)
{
    if (!error)
        error = hw_wait(hw, NK60_SPI1 + SPI_SR, SPI_BSY, 0, 1000);
    flash_select(hw, 0);
    return error;
}

static int flash_command(struct nk60_hw *hw, uint8_t command)
{
    flash_select(hw, 1);
    return flash_finish(hw, spi_byte(hw, command, NULL));
}

static int flash_status(struct nk60_hw *hw, uint8_t *status)
{
    int error;

    flash_select(hw, 1);
    error = spi_byte(hw, 0x05, NULL);
    if (!error)
        error = spi_byte(hw, 0xff, status);
    return flash_finish(hw, error);
}

static int flash_wait_ready(struct nk60_hw *hw, uint32_t timeout_us)
{
    uint32_t start = hw_time(hw);
    unsigned int iterations;

    for (iterations = 0; iterations < 100000u; iterations++) {
        uint8_t status;
        int error = flash_status(hw, &status);

        if (error)
            return error;
        if (!(status & FLASH_BUSY))
            return 0;
        if ((uint32_t)(hw_time(hw) - start) >= timeout_us)
            return -HW_ETIMEDOUT;
        flash_progress(hw);
        hw_delay(hw, 10);
    }
    return -HW_ETIMEDOUT;
}

static int flash_write_enable(struct nk60_hw *hw)
{
    uint8_t status;
    int error = flash_wait_ready(hw, FLASH_TIMEOUT_US);

    if (!error)
        error = flash_command(hw, 0x06);
    if (!error)
        error = flash_status(hw, &status);
    if (!error && !(status & FLASH_WEL))
        error = -HW_EIO;
    return error;
}

static int flash_address(struct nk60_hw *hw, uint8_t command, uint32_t offset)
{
    int error = spi_byte(hw, command, NULL);

    if (!error)
        error = spi_byte(hw, (uint8_t)(offset >> 16), NULL);
    if (!error)
        error = spi_byte(hw, (uint8_t)(offset >> 8), NULL);
    if (!error)
        error = spi_byte(hw, (uint8_t)offset, NULL);
    return error;
}

static int flash_range(struct nk60_hw *hw, uint32_t offset,
                        const void *buffer, size_t length)
{
    if (!hw_valid(hw) || (!buffer && length))
        return -HW_EINVAL;
    if (!hw->flash_ready)
        return -HW_ENODEV;
    if (offset > hw->flash_bytes || length > hw->flash_bytes - offset)
        return -HW_EINVAL;
    return 0;
}

int nk60_flash_init(struct nk60_hw *hw)
{
    unsigned int pin;
    int error;

    if (!hw_valid(hw) || !hw->io.read8 || !hw->io.write8)
        return -HW_EINVAL;
    hw->flash_ready = 0;
    hw->flash_bytes = 0;
    hw_update(hw, NK60_RCC + RCC_AHB1ENR, 0, 1u << 1);
    hw_update(hw, NK60_RCC + RCC_APB2ENR, 0, 1u << 12);
    (void)hw_read(hw, NK60_RCC + RCC_APB2ENR);
    flash_select(hw, 0);
    hw_gpio_config(hw, NK60_GPIO_B, 2, 1, 1, 0);
    for (pin = 3; pin <= 5; pin++)
        hw_gpio_config(hw, NK60_GPIO_B, pin, 2, 1, 5);
    hw_write(hw, NK60_SPI1 + SPI_CR1, 0);
    hw_write(hw, NK60_SPI1 + SPI_CR2, 0);
    /* APB2=84 MHz, /4 =>21 MHz; mode3, software NSS, 8-bit full duplex. */
    hw_write(hw, NK60_SPI1 + SPI_CR1,
             (1u << 9) | (1u << 8) | SPI_SPE | (1u << 3) | (1u << 2) | 3u);
    error = flash_command(hw, 0xab);
    if (error)
        return error;
    hw_delay(hw, 3);
    return nk60_flash_probe(hw, NULL);
}

int nk60_flash_probe(struct nk60_hw *hw, uint32_t *jedec_id)
{
    uint8_t manufacturer = 0, type = 0, density = 0;
    int error;

    if (!hw_valid(hw) || !hw->io.read8 || !hw->io.write8)
        return -HW_EINVAL;
    hw->flash_ready = 0;
    hw->flash_bytes = 0;
    flash_select(hw, 1);
    error = spi_byte(hw, 0x9f, NULL);
    if (!error)
        error = spi_byte(hw, 0xff, &manufacturer);
    if (!error)
        error = spi_byte(hw, 0xff, &type);
    if (!error)
        error = spi_byte(hw, 0xff, &density);
    error = flash_finish(hw, error);
    if (error)
        return error;
    if (jedec_id)
        *jedec_id = ((uint32_t)manufacturer << 16) | ((uint32_t)type << 8) | density;
    /* 0x14..0x18 are 1..16 MiB, all addressable with 24-bit commands. */
    if (manufacturer != 0xef || (type != 0x40 && type != 0x60) ||
        density < 0x14 || density > 0x18)
        return -HW_ENODEV;
    hw->flash_bytes = 1u << density;
    hw->flash_ready = 1;
    return 0;
}

int nk60_flash_read(struct nk60_hw *hw, uint32_t offset, void *data, size_t length)
{
    uint8_t *bytes = data;
    int error = flash_range(hw, offset, data, length);

    if (error || !length)
        return error;
    error = flash_wait_ready(hw, FLASH_TIMEOUT_US);
    if (error)
        return error;
    while (length) {
        size_t chunk = length > 64u ? 64u : length;
        size_t i;

        flash_select(hw, 1);
        error = flash_address(hw, 0x03, offset);
        for (i = 0; !error && i < chunk; i++)
            error = spi_byte(hw, 0xff, &bytes[i]);
        error = flash_finish(hw, error);
        if (error)
            return error;
        flash_progress(hw);
        bytes += chunk;
        offset += (uint32_t)chunk;
        length -= chunk;
    }
    return 0;
}

int nk60_flash_program(struct nk60_hw *hw, uint32_t offset,
                       const void *data, size_t length)
{
    const uint8_t *bytes = data;
    int error = flash_range(hw, offset, data, length);

    if (error)
        return error;
    while (length) {
        size_t chunk = NK60_FLASH_PAGE_SIZE - (offset % NK60_FLASH_PAGE_SIZE);
        size_t i;

        if (chunk > length)
            chunk = length;
        error = flash_write_enable(hw);
        if (error)
            return error;
        flash_select(hw, 1);
        error = flash_address(hw, 0x02, offset);
        for (i = 0; !error && i < chunk; i++)
            error = spi_byte(hw, bytes[i], NULL);
        error = flash_finish(hw, error);
        if (!error)
            error = flash_wait_ready(hw, FLASH_TIMEOUT_US);
        if (error)
            return error;
        /* Check persistence rather than report a protected/failed write as OK. */
        for (i = 0; i < chunk; ) {
            uint8_t actual[64];
            size_t part = chunk - i;
            size_t j;

            if (part > sizeof(actual))
                part = sizeof(actual);
            error = nk60_flash_read(hw, offset + (uint32_t)i, actual, part);
            if (error)
                return error;
            for (j = 0; j < part; j++) {
                if (actual[j] != bytes[i + j])
                    return -HW_EIO;
            }
            i += part;
        }
        offset += (uint32_t)chunk;
        bytes += chunk;
        length -= chunk;
    }
    return 0;
}

int nk60_flash_erase_sector(struct nk60_hw *hw, uint32_t offset)
{
    int error = flash_range(hw, offset, NULL, 0);

    if (error)
        return error;
    if ((offset % NK60_FLASH_SECTOR_SIZE) ||
        offset > hw->flash_bytes - NK60_FLASH_SECTOR_SIZE)
        return -HW_EINVAL;
    error = flash_write_enable(hw);
    if (error)
        return error;
    flash_select(hw, 1);
    error = flash_address(hw, 0x20, offset);
    error = flash_finish(hw, error);
    if (!error)
        error = flash_wait_ready(hw, FLASH_ERASE_TIMEOUT_US);
    if (!error) {
        uint32_t i;

        for (i = 0; i < NK60_FLASH_SECTOR_SIZE; i += 64u) {
            uint8_t actual[64];
            unsigned int j;

            error = nk60_flash_read(hw, offset + i, actual, sizeof(actual));
            if (error)
                return error;
            for (j = 0; j < sizeof(actual); j++) {
                if (actual[j] != 0xff)
                    return -HW_EIO;
            }
        }
    }
    return error;
}
