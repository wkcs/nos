/* Copyright (C) 2026 NOS contributors. */
#include "config.h"
#include "layout.h"
int kb_layout_position(unsigned key, struct kb_key_position *p)
{
    static const uint8_t counts[5] = {14, 14, 13, 12, 8};
    static const uint8_t first_width[5] = {4, 6, 7, 9, 5};
    static const uint8_t last_width[5] = {8, 6, 9, 11, 5};
    unsigned first = 0, row = 0, column, x = 0, width;
    if (!p || key >= KB_KEYS)
        return -KB_EINVAL;
    while (key >= first + counts[row])
        first += counts[row++];
    column = key - first;
    for (unsigned i = 0; i <= column; ++i) {
        width = row == 4               ? (i == 3 ? 25 : 5)
                : i == 0               ? first_width[row]
                : i + 1 == counts[row] ? last_width[row]
                                       : 4;
        if (i == column)
            break;
        x += width;
    }
    *p = (struct kb_key_position){x, (uint8_t)(row * 4), width, 4, (uint8_t)key};
    return 0;
}
