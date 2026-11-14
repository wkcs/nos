/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NK60_USB_RX_FIFO_H
#define NK60_USB_RX_FIFO_H

#include <stdint.h>

static inline uint16_t nk60_usb_read_rx_fifo(volatile const uint32_t *fifo,
                                            uint8_t *dest, uint16_t packet_size,
                                            uint32_t capacity)
{
    uint16_t copied = dest != 0 ? packet_size : 0U;
    uint16_t offset;

    if (copied > capacity) {
        copied = (uint16_t)capacity;
    }
    /* The FIFO advances a word per read, including bytes discarded by the receiver. */
    for (offset = 0U; offset < packet_size; offset += 4U) {
        uint32_t word = *fifo;
        uint16_t i;

        for (i = 0U; i < 4U && offset + i < copied; i++) {
            dest[offset + i] = (uint8_t)(word >> (8U * i));
        }
    }
    return copied;
}

#endif
