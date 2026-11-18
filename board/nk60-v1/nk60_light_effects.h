/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * NK60 lighting renderers and their registration order.
 */
#ifndef NK60_LIGHT_EFFECTS_H
#define NK60_LIGHT_EFFECTS_H

#include <nos/keyboard.h>

/* Phase advances by speed + 1 per 30 ms; powers of two also wrap at UINT32_MAX. */
#define NK60_EFFECT_PERIOD 8192U
#define NK60_EFFECT_COLUMN_PHASE 512U
#define NK60_RAINBOW_COLUMN_PHASE 256U
#define NK60_BREATH_PERIOD 32768U

/* The order defines IDs for this device's default configuration. */
enum nk60_light_effect_id {
    NK60_EFFECT_STATIC,
    NK60_EFFECT_BREATH,
    NK60_EFFECT_RAINBOW,
    NK60_EFFECT_CYCLE,
    NK60_EFFECT_WAVE,
    NK60_EFFECT_COMET,
    NK60_EFFECT_GRADIENT,
    NK60_EFFECT_RIPPLE,
    NK60_EFFECT_TWINKLE,
    NK60_EFFECT_COUNT,
};

#define NK60_EFFECT_MASK ((1U << NK60_EFFECT_COUNT) - 1U)
_Static_assert(NK60_EFFECT_COUNT <= 16U, "NK60 effects must fit the zone bitmask");

#endif
