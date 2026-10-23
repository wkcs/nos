/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NK60_KEYBOARD_SERVICE_H
#define NOS_NK60_KEYBOARD_SERVICE_H

#include "keyboard.h"
#include "../../boards/nk60_v2/hardware.h"
#include "../../boards/nk60_v2/nk60_usb.h"

#define NK60_SERVICE_LINE_BYTES 96u
#define NK60_SERVICE_SCAN_US 4000u
#define NK60_SERVICE_SAVE_MS 60000u
#define NK60_SERVICE_TX_BYTES 1024u

struct nk60_service_hooks {
    void *context;
    /* Nonblocking input: an octet or a negative errno. */
    int (*read_byte)(void *context);
    /* Nonblocking output: zero after accepting all bytes, -EAGAIN to retry. */
    int (*write)(void *context, const char *bytes, size_t length);
    nk60_dma_tx_fn dma_tx;
};

/* Statically allocated by the trusted user-mode service, never on its stack. */
struct nk60_service {
    struct nk60_hw hardware;
    struct nk60_usb usb;
    struct keyboard keyboard;
    struct kb_config candidate;
    struct kb_store store;
    struct nk60_service_hooks hooks;
    struct nk60_rgb colors[NK60_LED_COUNT];
    uint8_t wave[NK60_RGB_WAVE_BYTES];
    char line[NK60_SERVICE_LINE_BYTES];
    char tx[NK60_SERVICE_TX_BYTES];
    uint64_t pressed;
    uint32_t last_us, now_ms, fraction_us, next_scan_us;
    uint32_t last_edit_ms, last_rgb_ms, unlock_started_ms, unlocked_ms;
    uint32_t generation, store_base, edits, scan_overruns, scan_errors;
    uint32_t usb_errors, save_errors, rgb_errors;
    uint32_t tx_dropped;
    uint16_t tx_head, tx_count;
    int usb_error;
    uint8_t line_length, line_overflow, swallow_lf;
    uint8_t dirty, save_requested, saving, progress_busy;
    uint8_t usb_ready, matrix_ready, flash_ready, rgb_ready;
    uint8_t configured, suspended, unlock_held, unlocked;
    uint8_t rgb_pending, last_leds, wake_requested;
};

/* Returns zero when scan+USB are ready; optional flash/RGB failures remain
 * observable through status. No function claims that physical I/O was tested. */
int nk60_service_init(struct nk60_service *, const struct nk60_io *,
                      const struct nk60_service_hooks *);
void nk60_service_step(struct nk60_service *);
/* UART command parser can be driven independently of the endless task loop. */
void nk60_service_command(struct nk60_service *, const char *line);
void nos_keyboard_service(uintptr_t unused);
#endif
