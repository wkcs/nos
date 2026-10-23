/* Copyright (C) 2026 NOS contributors. */
#include "nlp/text.h"
#include "nlp/registry.h"
int nlp_utf8_prefix(const uint8_t *p, size_t size, size_t limit, size_t *written)
{
    size_t at = 0, end = 0;
    if ((!p && size) || !written || size > 4096)
        return -NLP_INVALID;
    while (at < size) {
        uint32_t value = p[at];
        unsigned count;
        if (value < 0x80)
            count = 1;
        else if (value >= 0xc2 && value <= 0xdf) {
            count = 2;
            value &= 31;
        } else if (value >= 0xe0 && value <= 0xef) {
            count = 3;
            value &= 15;
        } else if (value >= 0xf0 && value <= 0xf4) {
            count = 4;
            value &= 7;
        } else
            return -NLP_INVALID;
        if (count > size - at)
            return -NLP_INVALID;
        for (unsigned i = 1; i < count; ++i) {
            if ((p[at + i] & 0xc0) != 0x80)
                return -NLP_INVALID;
            value = value << 6 | (p[at + i] & 63);
        }
        if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
            (count == 4 && value < 0x10000) || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff))
            return -NLP_INVALID;
        at += count;
        if (at <= limit)
            end = at;
    }
    *written = end;
    return 0;
}
