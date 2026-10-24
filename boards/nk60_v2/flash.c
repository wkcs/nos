/* Copyright (C) 2023 胡启航<Nick Hu>, huqihan@live.com
 * Copyright (C) 2026 NOS contributors.
 * NK60 wiring adapter. W25Q protocol lives in servers/drivers/spi/nor.c. */
#include "hw_internal.h"

int nk60_flash_init(struct nk60_hw *hw)
{
    int error;
    if (!hw_valid(hw) || !hw->io.read8 || !hw->io.write8)
        return -HW_EINVAL;
    hw->flash_ready = 0;
    hw->flash_bytes = 0;
    error = nk60_spi_start(hw);
    return error ? error : nk60_flash_probe(hw, NULL);
}
int nk60_flash_probe(struct nk60_hw *hw, uint32_t *id)
{
    int error;
    if (!hw_valid(hw))
        return -HW_EINVAL;
    error = nos_spi_nor_probe(&hw->nor, id);
    hw->flash_ready = hw->nor.ready;
    hw->flash_bytes = hw->nor.capacity;
    return error;
}
int nk60_flash_read(struct nk60_hw *hw, uint32_t offset, void *data, size_t count)
{
    return !hw_valid(hw) ? -HW_EINVAL : nos_spi_nor_read(&hw->nor, offset, data, count);
}
int nk60_flash_program(struct nk60_hw *hw, uint32_t offset, const void *data, size_t count)
{
    return !hw_valid(hw) ? -HW_EINVAL : nos_spi_nor_program(&hw->nor, offset, data, count);
}
int nk60_flash_erase_sector(struct nk60_hw *hw, uint32_t offset)
{
    return !hw_valid(hw) ? -HW_EINVAL : nos_spi_nor_erase(&hw->nor, offset);
}
