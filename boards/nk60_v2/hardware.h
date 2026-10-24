/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NK60_V2_HARDWARE_H
#define NOS_NK60_V2_HARDWARE_H

#include <stddef.h>
#include <stdint.h>
#include "driver/spi_nor.h"

#define NK60_KEY_COUNT 61u
#define NK60_SCAN_SLOTS 72u
#define NK60_LED_COUNT 61u
#define NK60_CANVAS_WIDTH 14u
#define NK60_CANVAS_HEIGHT 5u
#define NK60_RGB_WAVE_BYTES (NK60_LED_COUNT * 24u)
#define NK60_FLASH_SECTOR_SIZE 4096u
#define NK60_FLASH_PAGE_SIZE 256u

#define NK60_GPIO_A ((uintptr_t)0x40020000u)
#define NK60_GPIO_B ((uintptr_t)0x40020400u)
#define NK60_GPIO_C ((uintptr_t)0x40020800u)
#define NK60_GPIO_D ((uintptr_t)0x40020c00u)
#define NK60_RCC ((uintptr_t)0x40023800u)
#define NK60_SPI1 ((uintptr_t)0x40013000u)
#define NK60_SPI2 ((uintptr_t)0x40003800u)
#define NK60_SPI_DR 0x0cu

/*
 * Callbacks belong to the trusted user-mode device service. They must perform
 * only authorized device accesses. time_us is a monotonically increasing,
 * wrapping uint32_t microsecond counter; delay_us must not return early.
 * Register polling also has an iteration limit if the time source stalls.
 */
struct nk60_io {
    void *context;
    uint32_t (*read32)(void *context, uintptr_t address);
    void (*write32)(void *context, uintptr_t address, uint32_t value);
    uint8_t (*read8)(void *context, uintptr_t address);
    void (*write8)(void *context, uintptr_t address, uint8_t value);
    uint32_t (*time_us)(void *context);
    void (*delay_us)(void *context, uint32_t microseconds);
};

/* After bind, this object contains self references: never move or copy it. */
struct nk60_hw {
    struct nk60_io io;
    struct nos_device gpio_device, spi_device;
    struct nos_gpio_controller gpio;
    struct nos_spi_bus spi;
    struct nos_spi_nor nor;
    uint16_t gpio_output[3];
    /* Optional USB/event progress, called only with flash CS deasserted. */
    void (*progress)(void *context);
    void *progress_context;
    uint32_t flash_bytes;
    uint8_t flash_ready;
    uint8_t matrix_ready;
    uint8_t rgb_ready;
    uint8_t rgb_powered;
};

struct nk60_rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

/*
 * DMA must continuously send bytes to the supplied SPI data register, block
 * until transfer completion, and stop DMA on every error/timeout. The service
 * must validate/pin the source buffer and enforce DMA1 stream4/channel0 bounds.
 * SPI shift-register draining and the LED reset interval are handled here.
 */
typedef int (*nk60_dma_tx_fn)(void *context, uintptr_t destination, const uint8_t *source,
                              size_t length, uint32_t timeout_us);

extern const int8_t nk60_scan_to_key[NK60_SCAN_SLOTS];
extern const uint8_t nk60_canvas_to_led[NK60_CANVAS_WIDTH * NK60_CANVAS_HEIGHT];

/* All errors are negative POSIX errno values; no allocation or RTOS is used. */
int nk60_hw_bind(struct nk60_hw *hw, const struct nk60_io *io);
/* Progress must not reenter any flash function or change SPI1 configuration. */
void nk60_hw_set_progress(struct nk60_hw *hw, void (*progress)(void *), void *context);
int nk60_matrix_init(struct nk60_hw *hw);
int nk60_matrix_scan(struct nk60_hw *hw, uint64_t *pressed);
int nk60_flash_init(struct nk60_hw *hw);
int nk60_flash_probe(struct nk60_hw *hw, uint32_t *jedec_id);
int nk60_flash_read(struct nk60_hw *hw, uint32_t offset, void *data, size_t length);
/* Program only previously erased bytes; erase is always a separate operation. */
int nk60_flash_program(struct nk60_hw *hw, uint32_t offset, const void *data, size_t length);
int nk60_flash_erase_sector(struct nk60_hw *hw, uint32_t offset);
int nk60_rgb_init(struct nk60_hw *hw);
int nk60_rgb_set_power(struct nk60_hw *hw, int enabled);
int nk60_rgb_encode(const struct nk60_rgb *colors, size_t count, uint8_t *wave, size_t wave_bytes);
int nk60_rgb_write(struct nk60_hw *hw, const uint8_t *wave, size_t wave_bytes,
                   nk60_dma_tx_fn dma_tx, void *dma_context);

#endif
