/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_DRIVER_SPI_NOR_H
#define NOS_DRIVER_SPI_NOR_H
#include "peripheral.h"
#define NOS_SPI_NOR_SECTOR 4096u
#define NOS_SPI_NOR_PAGE 256u
/* W25Q 24-bit JEDEC subset; no SFDP/QSPI/32-bit address claims. */
struct nos_spi_nor {
    struct nos_spi_device spi;
    void *context;
    uint32_t (*time_us)(void *);
    void (*delay_us)(void *, uint32_t);
    /* Called only BETWEEN transactions with CS deasserted. Must not reenter
     * this NOR or change the bus configuration. */
    void (*progress)(void *);
    uint32_t capacity;
    uint8_t ready;
};
int nos_spi_nor_probe(struct nos_spi_nor *, uint32_t *jedec_id);
int nos_spi_nor_read(struct nos_spi_nor *, uint32_t offset, void *, size_t);
int nos_spi_nor_program(struct nos_spi_nor *, uint32_t offset, const void *, size_t);
int nos_spi_nor_erase(struct nos_spi_nor *, uint32_t offset);
#endif
