/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVERS_W25Q_FLASH_H__
#define __NOS_DRIVERS_W25Q_FLASH_H__

#include <drivers/w25q_spi.h>
#include <kernel/mutex.h>

struct w25q_flash {
    const struct w25q_spi *spi;
    uint32_t capacity;
    struct mutex lock;
};

int w25q_flash_init(struct w25q_flash *flash, const struct w25q_spi *spi,
                    uint32_t capacity);
int w25q_flash_read(struct w25q_flash *flash, uint32_t address,
                    void *buffer, size_t length);
int w25q_flash_write(struct w25q_flash *flash, uint32_t address,
                     const void *buffer, size_t length);

#endif /* __NOS_DRIVERS_W25Q_FLASH_H__ */
