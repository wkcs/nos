/* Copyright (C) 2026 NOS contributors */
#include <string.h>
#include "config.h"

static void key(struct kb_config *c, unsigned layer, unsigned position, unsigned usage)
{
    c->map[layer][position] = (struct kb_action){ KB_KEY, 0, (uint16_t)usage };
}

void kb_default_config(struct kb_config *c)
{
    static const uint8_t base[KB_KEYS] = {
        KB_ESCAPE, KB_1, KB_2, KB_3, KB_4, KB_5, KB_6, KB_7, KB_8,
        KB_9, KB_0, KB_MINUS, KB_EQUAL, KB_BACKSPACE,
        KB_TAB, KB_Q, KB_W, KB_E, KB_R, KB_T, KB_Y, KB_U, KB_I,
        KB_O, KB_P, KB_LBRACKET, KB_RBRACKET, KB_BACKSLASH,
        KB_CAPS, KB_A, KB_S, KB_D, KB_F, KB_G, KB_H, KB_J, KB_K,
        KB_L, KB_SEMICOLON, KB_QUOTE, KB_ENTER,
        KB_LSHIFT, KB_Z, KB_X, KB_C, KB_V, KB_B, KB_N, KB_M,
        KB_COMMA, KB_DOT, KB_SLASH, KB_RSHIFT,
        KB_LCTRL, KB_LGUI, KB_LALT, KB_SPACE, KB_RALT, 0, KB_RCTRL, 0
    };
    unsigned l, i;
    memset(c, 0, sizeof(*c));
    c->debounce_ms = 5;
    c->tapping_ms = 200;
    c->macro_ms = 12;
    c->layers = KB_LAYERS;
    c->nkro = 1;
    c->brightness = 128;
    for (i = 0; i < KB_KEYS; ++i)
        if (base[i])
            key(c, 0, i, base[i]);
    c->map[0][58] = (struct kb_action){ KB_MOMENTARY, KB_LAYER_PN, 0 };
    c->map[0][60] = (struct kb_action){ KB_MOMENTARY, KB_LAYER_FN, 0 };
    for (l = 1; l < KB_LAYERS; ++l)
        for (i = 0; i < KB_KEYS; ++i)
            c->map[l][i].type = KB_TRANSPARENT;
    for (l = KB_LAYER_PN; l <= KB_LAYER_FN; ++l) {
        key(c, l, 0, KB_GRAVE);
        for (i = 1; i <= 12; ++i)
            key(c, l, i, KB_F1 + i - 1);
        key(c, l, 13, KB_DELETE);
        key(c, l, 16, KB_UP);
        key(c, l, 27, KB_INSERT);
        key(c, l, 29, KB_LEFT);
        key(c, l, 30, KB_DOWN);
        key(c, l, 31, KB_RIGHT);
        key(c, l, 34, KB_PRINT);
        key(c, l, 35, KB_SCROLL);
        key(c, l, 36, KB_PAUSE);
        key(c, l, 39, KB_END);
    }
    key(c, KB_LAYER_FN, 49, KB_PAGEUP);
    key(c, KB_LAYER_FN, 50, KB_PAGEDOWN);
    key(c, KB_LAYER_FN, 51, KB_UP);
    key(c, KB_LAYER_FN, 57, KB_LEFT);
    key(c, KB_LAYER_FN, 58, KB_DOWN);
    key(c, KB_LAYER_FN, 59, KB_RIGHT);
    c->map[KB_LAYER_PN][47] = (struct kb_action){ KB_LOCAL, KB_LIGHT_NEXT, 0 };
    c->map[KB_LAYER_FN][46] = (struct kb_action){ KB_CONSUMER, 0, KB_AUDIO_DOWN };
    c->map[KB_LAYER_FN][47] = (struct kb_action){ KB_CONSUMER, 0, KB_AUDIO_UP };
    c->map[KB_LAYER_FN][48] = (struct kb_action){ KB_CONSUMER, 0, KB_AUDIO_MUTE };
}
