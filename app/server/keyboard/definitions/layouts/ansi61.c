/** Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * ANSI geometry uses hundredths of one key unit; indices follow NK60 scan/LED order.
 */
#include <nos/keyboard_definitions.h>

static const uint8_t kb_ansi61_default_keymap[3][61] = {
    KEYMAP(
        ESC, 1,   2,   3,   4,   5,   6,   7,   8,   9,   0,   MINS,EQL, BSPC, \
        TAB, Q,   W,   E,   R,   T,   Y,   U,   I,   O,   P,   LBRC,RBRC,BSLS, \
        CAPS,A,   S,   D,   F,   G,   H,   J,   K,   L,   SCLN,QUOT,     ENT,  \
        LSFT,Z,   X,   C,   V,   B,   N,   M,   COMM,DOT, SLSH,          RSFT, \
        LCTL,LGUI,LALT,          SPC,                     RALT, PN, RCTL,FN    ),
    KEYMAP(
        GRV, F1,  F2,  F3,  F4,  F5,  F6,  F7,  F8,  F9,  F10, F11, F12, DEL, \
        NO,  NO , UP,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  INS, \
        NO,  LEFT,DOWN,RGHT,NO,  NO,  PSCR,SLCK,PAUS,NO,  NO,  END,      NO,  \
        NO,  NO , NO,  NO,  NO,  NO,  NO,  NO,  PGUP,PGDN,UP,            NO,  \
        NO,  NO,  NO,            NO,                      LEFT,DOWN,RGHT,NO   ),
    KEYMAP(
        GRV, F1,  F2,  F3,  F4,  F5,  F6,  F7,  F8,  F9,  F10, F11, F12, DEL, \
        NO,  NO , UP,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  NO,  INS, \
        NO,  LEFT,DOWN,RGHT,NO,  NO,  PSCR,SLCK,PAUS,NO,  NO,  END,      NO,  \
        NO,  NO , NO,  NO,  NO,  NO,  NO,  NO,  NO  ,NO  ,NO,            NO,  \
        NO,  NO,  NO,            NO,                      NO,  NO,  NO,  NO   ),
};

static const struct kb_key_geometry kb_ansi61_keys[] = {
    { 0U, 0U, 100U, 100U, 0U, 0U, 0U },
    { 100U, 0U, 100U, 100U, 0U, 1U, 1U },
    { 200U, 0U, 100U, 100U, 0U, 2U, 2U },
    { 300U, 0U, 100U, 100U, 0U, 3U, 3U },
    { 400U, 0U, 100U, 100U, 0U, 4U, 4U },
    { 500U, 0U, 100U, 100U, 0U, 5U, 5U },
    { 600U, 0U, 100U, 100U, 0U, 6U, 6U },
    { 700U, 0U, 100U, 100U, 0U, 7U, 7U },
    { 800U, 0U, 100U, 100U, 0U, 8U, 8U },
    { 900U, 0U, 100U, 100U, 0U, 9U, 9U },
    { 1000U, 0U, 100U, 100U, 0U, 10U, 10U },
    { 1100U, 0U, 100U, 100U, 0U, 11U, 11U },
    { 1200U, 0U, 100U, 100U, 0U, 12U, 12U },
    { 1300U, 0U, 200U, 100U, 0U, 13U, 13U },
    { 0U, 100U, 150U, 100U, 1U, 0U, 14U },
    { 150U, 100U, 100U, 100U, 1U, 1U, 15U },
    { 250U, 100U, 100U, 100U, 1U, 2U, 16U },
    { 350U, 100U, 100U, 100U, 1U, 3U, 17U },
    { 450U, 100U, 100U, 100U, 1U, 4U, 18U },
    { 550U, 100U, 100U, 100U, 1U, 5U, 19U },
    { 650U, 100U, 100U, 100U, 1U, 6U, 20U },
    { 750U, 100U, 100U, 100U, 1U, 7U, 21U },
    { 850U, 100U, 100U, 100U, 1U, 8U, 22U },
    { 950U, 100U, 100U, 100U, 1U, 9U, 23U },
    { 1050U, 100U, 100U, 100U, 1U, 10U, 24U },
    { 1150U, 100U, 100U, 100U, 1U, 11U, 25U },
    { 1250U, 100U, 100U, 100U, 1U, 12U, 26U },
    { 1350U, 100U, 150U, 100U, 1U, 13U, 27U },
    { 0U, 200U, 175U, 100U, 2U, 0U, 28U },
    { 175U, 200U, 100U, 100U, 2U, 1U, 29U },
    { 275U, 200U, 100U, 100U, 2U, 2U, 30U },
    { 375U, 200U, 100U, 100U, 2U, 3U, 31U },
    { 475U, 200U, 100U, 100U, 2U, 4U, 32U },
    { 575U, 200U, 100U, 100U, 2U, 5U, 33U },
    { 675U, 200U, 100U, 100U, 2U, 6U, 34U },
    { 775U, 200U, 100U, 100U, 2U, 7U, 35U },
    { 875U, 200U, 100U, 100U, 2U, 8U, 36U },
    { 975U, 200U, 100U, 100U, 2U, 9U, 37U },
    { 1075U, 200U, 100U, 100U, 2U, 10U, 38U },
    { 1175U, 200U, 100U, 100U, 2U, 11U, 39U },
    { 1275U, 200U, 225U, 100U, 2U, 13U, 40U },
    { 0U, 300U, 225U, 100U, 3U, 0U, 41U },
    { 225U, 300U, 100U, 100U, 3U, 1U, 42U },
    { 325U, 300U, 100U, 100U, 3U, 2U, 43U },
    { 425U, 300U, 100U, 100U, 3U, 3U, 44U },
    { 525U, 300U, 100U, 100U, 3U, 4U, 45U },
    { 625U, 300U, 100U, 100U, 3U, 5U, 46U },
    { 725U, 300U, 100U, 100U, 3U, 6U, 47U },
    { 825U, 300U, 100U, 100U, 3U, 7U, 48U },
    { 925U, 300U, 100U, 100U, 3U, 8U, 49U },
    { 1025U, 300U, 100U, 100U, 3U, 9U, 50U },
    { 1125U, 300U, 100U, 100U, 3U, 10U, 51U },
    { 1225U, 300U, 275U, 100U, 3U, 13U, 52U },
    { 0U, 400U, 125U, 100U, 4U, 0U, 53U },
    { 125U, 400U, 125U, 100U, 4U, 1U, 54U },
    { 250U, 400U, 125U, 100U, 4U, 2U, 55U },
    { 375U, 400U, 625U, 100U, 4U, 6U, 56U },
    { 1000U, 400U, 125U, 100U, 4U, 10U, 57U },
    { 1125U, 400U, 125U, 100U, 4U, 11U, 58U },
    { 1250U, 400U, 125U, 100U, 4U, 12U, 59U },
    { 1375U, 400U, 125U, 100U, 4U, 13U, 60U },
};

const struct kb_layout_definition kb_selected_layout = {
    .id = "ansi61", .version = 1U,
    .key_count = 61U, .layer_count = 3U, .rows = 5U, .cols = 14U,
    .keys = kb_ansi61_keys, .default_keymap = &kb_ansi61_default_keymap[0][0],
};
