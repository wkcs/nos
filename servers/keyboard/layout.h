/* Copyright (C) 2026 NOS contributors. ANSI 60% physical geometry, independent of scan wiring. */
#ifndef NOS_KEYBOARD_LAYOUT_H
#define NOS_KEYBOARD_LAYOUT_H
#include <stdint.h>
#define KB_LAYOUT_WIDTH 60u
#define KB_LAYOUT_HEIGHT 20u
struct kb_key_position {
    uint8_t x, y, width, height, led;
};
/* Coordinates and dimensions are in quarter-key units; key and LED IDs are stable. */
int kb_layout_position(unsigned key, struct kb_key_position *position);
#endif
