#ifndef __KEYBOARD_H__
#define __KEYBOARD_H__

#define IS_FN(code) ((code) == KC_FN)
#define IS_PN(code) ((code) == KC_PN)
#define IS_MODIFIERS(code) ((code) >= KC_LCTRL && (code) <= KC_PN)

/*
 * Short names for ease of definition of keymap
 */
#define KC_LCTL KC_LCTRL
#define KC_RCTL KC_RCTRL
#define KC_LSFT KC_LSHIFT
#define KC_RSFT KC_RSHIFT
#define KC_ESC  KC_ESCAPE
#define KC_BSPC KC_BSPACE
#define KC_ENT  KC_ENTER
#define KC_DEL  KC_DELETE
#define KC_INS  KC_INSERT
#define KC_CAPS KC_CAPSLOCK
#define KC_CLCK KC_CAPSLOCK
#define KC_RGHT KC_RIGHT
#define KC_PGDN KC_PGDOWN
#define KC_PSCR KC_PSCREEN
#define KC_SLCK KC_SCROLLLOCK
#define KC_PAUS KC_PAUSE
#define KC_BRK  KC_PAUSE
#define KC_NLCK KC_NUMLOCK
#define KC_SPC  KC_SPACE
#define KC_MINS KC_MINUS
#define KC_EQL  KC_EQUAL
#define KC_GRV  KC_GRAVE
#define KC_RBRC KC_RBRACKET
#define KC_LBRC KC_LBRACKET
#define KC_COMM KC_COMMA
#define KC_BSLS KC_BSLASH
#define KC_SLSH KC_SLASH
#define KC_SCLN KC_SCOLON
#define KC_QUOT KC_QUOTE
#define KC_APP  KC_APPLICATION
#define KC_NUHS KC_NONUS_HASH
#define KC_NUBS KC_NONUS_BSLASH
#define KC_LCAP KC_LOCKING_CAPS
#define KC_LNUM KC_LOCKING_NUM
#define KC_LSCR KC_LOCKING_SCROLL
#define KC_ERAS KC_ALT_ERASE
#define KC_CLR  KC_CLEAR
/* Japanese specific */
#define KC_ZKHK KC_GRAVE
#define KC_RO   KC_INT1
#define KC_KANA KC_INT2
#define KC_JYEN KC_INT3
#define KC_JPY  KC_INT3
#define KC_HENK KC_INT4
#define KC_MHEN KC_INT5
/* Korean specific */
#define KC_HAEN KC_LANG1
#define KC_HANJ KC_LANG2
/* Keypad */
#define KC_P1   KC_KP_1
#define KC_P2   KC_KP_2
#define KC_P3   KC_KP_3
#define KC_P4   KC_KP_4
#define KC_P5   KC_KP_5
#define KC_P6   KC_KP_6
#define KC_P7   KC_KP_7
#define KC_P8   KC_KP_8
#define KC_P9   KC_KP_9
#define KC_P0   KC_KP_0
#define KC_P00  KC_KP_00
#define KC_P000 KC_KP_000
#define KC_PDOT KC_KP_DOT
#define KC_PCMM KC_KP_COMMA
#define KC_PSLS KC_KP_SLASH
#define KC_PAST KC_KP_ASTERISK
#define KC_PMNS KC_KP_MINUS
#define KC_PPLS KC_KP_PLUS
#define KC_PEQL KC_KP_EQUAL
#define KC_PENT KC_KP_ENTER
/* Unix function key */
#define KC_EXEC KC_EXECUTE
#define KC_SLCT KC_SELECT
#define KC_AGIN KC_AGAIN
#define KC_PSTE KC_PASTE
/* Mousekey */
#define KC_MS_U KC_MS_UP
#define KC_MS_D KC_MS_DOWN
#define KC_MS_L KC_MS_LEFT
#define KC_MS_R KC_MS_RIGHT
#define KC_BTN1 KC_MS_BTN1
#define KC_BTN2 KC_MS_BTN2
#define KC_BTN3 KC_MS_BTN3
#define KC_BTN4 KC_MS_BTN4
#define KC_BTN5 KC_MS_BTN5
#define KC_WH_U KC_MS_WH_UP
#define KC_WH_D KC_MS_WH_DOWN
#define KC_WH_L KC_MS_WH_LEFT
#define KC_WH_R KC_MS_WH_RIGHT
#define KC_ACL0 KC_MS_ACCEL0
#define KC_ACL1 KC_MS_ACCEL1
#define KC_ACL2 KC_MS_ACCEL2

enum key_code {
    KC_NO                   = 0x00,
    KC_ROLL_OVER,           // 01
    KC_POST_FAIL,           // 02
    KC_UNDEFINED,           // 03
    KC_A,                   // 04
    KC_B,                   // 05
    KC_C,                   // 06
    KC_D,                   // 07
    KC_E,                   // 08
    KC_F,                   // 09
    KC_G,                   // 0A
    KC_H,                   // 0B
    KC_I,                   // 0C
    KC_J,                   // 0D
    KC_K,                   // 0E
    KC_L,                   // 0F
    KC_M,                   // 10
    KC_N,                   // 11
    KC_O,                   // 12
    KC_P,                   // 13
    KC_Q,                   // 14
    KC_R,                   // 15
    KC_S,                   // 16
    KC_T,                   // 17
    KC_U,                   // 18
    KC_V,                   // 19
    KC_W,                   // 1A
    KC_X,                   // 1B
    KC_Y,                   // 1C
    KC_Z,                   // 1D
    KC_1,                   // 1E
    KC_2,                   // 1F
    KC_3,                   // 20
    KC_4,                   // 21
    KC_5,                   // 22
    KC_6,                   // 23
    KC_7,                   // 24
    KC_8,                   // 25
    KC_9,                   // 26
    KC_0,                   // 27
    KC_ENTER,               // 28
    KC_ESCAPE,              // 29
    KC_BSPACE,              // 2A
    KC_TAB,                 // 2B
    KC_SPACE,               // 2C
    KC_MINUS,               // 2D
    KC_EQUAL,               // 2E
    KC_LBRACKET,            // 2F
    KC_RBRACKET,            // 30
    KC_BSLASH,              // 31   \ (and |)
    KC_NONUS_HASH,          // 32   Non-US # and ~ (Typically near the Enter key)
    KC_SCOLON,              // 33   ; (and :)
    KC_QUOTE,               // 34   ' and "
    KC_GRAVE,               // 35   Grave accent and tilde
    KC_COMMA,               // 36   , and <
    KC_DOT,                 // 37   . and >
    KC_SLASH,               // 38   / and ?
    KC_CAPSLOCK,            // 39
    KC_F1,                  // 3A
    KC_F2,                  // 3B
    KC_F3,                  // 3C
    KC_F4,                  // 3D
    KC_F5,                  // 3E
    KC_F6,                  // 3F
    KC_F7,                  // 40
    KC_F8,                  // 41
    KC_F9,                  // 42
    KC_F10,                 // 43
    KC_F11,                 // 44
    KC_F12,                 // 45
    KC_PSCREEN,             // 46
    KC_SCROLLLOCK,          // 47
    KC_PAUSE,               // 48
    KC_INSERT,              // 49
    KC_HOME,                // 4A
    KC_PGUP,                // 4B
    KC_DELETE,              // 4C
    KC_END,                 // 4D
    KC_PGDOWN,              // 4E
    KC_RIGHT,               // 4F
    KC_LEFT,                // 50
    KC_DOWN,                // 51
    KC_UP,                  // 52
    KC_NUMLOCK,             // 53
    KC_KP_SLASH,            // 54
    KC_KP_ASTERISK,         // 55
    KC_KP_MINUS,            // 56
    KC_KP_PLUS,             // 57
    KC_KP_ENTER,            // 58
    KC_KP_1,                // 59
    KC_KP_2,                // 5A
    KC_KP_3,                // 5B
    KC_KP_4,                // 5C
    KC_KP_5,                // 5D
    KC_KP_6,                // 5E
    KC_KP_7,                // 5F
    KC_KP_8,                // 60
    KC_KP_9,                // 61
    KC_KP_0,                // 62
    KC_KP_DOT,              // 63
    KC_NONUS_BSLASH,        // 64   Non-US \ and | (Typically near the Left-Shift key) */
    KC_APPLICATION,         // 65
    KC_POWER,               // 66
    KC_KP_EQUAL,            // 67
    KC_F13,                 // 68
    KC_F14,                 // 69
    KC_F15,                 // 6A
    KC_F16,                 // 6B
    KC_F17,                 // 6C
    KC_F18,                 // 6D
    KC_F19,                 // 6E
    KC_F20,                 // 6F
    KC_F21,                 // 70
    KC_F22,                 // 71
    KC_F23,                 // 72
    KC_F24,                 // 73
    KC_EXECUTE,             // 74
    KC_HELP,                // 75
    KC_MENU,                // 76
    KC_SELECT,              // 77
    KC_STOP,                // 78
    KC_AGAIN,               // 79
    KC_UNDO,                // 7A
    KC_CUT,                 // 7B
    KC_COPY,                // 7C
    KC_PASTE,               // 7D
    KC_FIND,                // 7E
    KC__MUTE,               // 7F
    KC__VOLUP,              // 80
    KC__VOLDOWN,            // 81
    KC_LOCKING_CAPS,        // 82   locking Caps Lock */
    KC_LOCKING_NUM,         // 83   locking Num Lock */
    KC_LOCKING_SCROLL,      // 84   locking Scroll Lock */
    KC_KP_COMMA,            // 85
    KC_KP_EQUAL_AS400,      // 86   equal sign on AS/400 */
    KC_INT1,                // 87
    KC_INT2,                // 88
    KC_INT3,                // 89
    KC_INT4,                // 8A
    KC_INT5,                // 8B
    KC_INT6,                // 8C
    KC_INT7,                // 8D
    KC_INT8,                // 8E
    KC_INT9,                // 8F
    KC_LANG1,               // 90
    KC_LANG2,               // 91
    KC_LANG3,               // 92
    KC_LANG4,               // 93
    KC_LANG5,               // 94
    KC_LANG6,               // 95
    KC_LANG7,               // 96
    KC_LANG8,               // 97
    KC_LANG9,               // 98
    KC_ALT_ERASE,           // 99
    KC_SYSREQ,              // 9A
    KC_CANCEL,              // 9B
    KC_CLEAR,               // 9C
    KC_PRIOR,               // 9D
    KC_RETURN,              // 9E
    KC_SEPARATOR,           // 9F
    KC_OUT,                 // A0
    KC_OPER,                // A1
    KC_CLEAR_AGAIN,         // A2
    KC_CRSEL,               // A3
    KC_EXSEL,               // A4

    /* NOTE: Following code range(0xB0-DD) are shared with special codes of 8-bit keymap */
    KC_KP_00                = 0xB0,
    KC_KP_000,              // B1
    KC_THOUSANDS_SEPARATOR, // B2
    KC_DECIMAL_SEPARATOR,   // B3
    KC_CURRENCY_UNIT,       // B4
    KC_CURRENCY_SUB_UNIT,   // B5
    KC_KP_LPAREN,           // B6
    KC_KP_RPAREN,           // B7
    KC_KP_LCBRACKET,        // B8   {
    KC_KP_RCBRACKET,        // B9   }
    KC_KP_TAB,              // BA
    KC_KP_BSPACE,           // BB
    KC_KP_A,                // BC
    KC_KP_B,                // BD
    KC_KP_C,                // BE
    KC_KP_D,                // BF
    KC_KP_E,                // C0
    KC_KP_F,                // C1
    KC_KP_XOR,              // C2
    KC_KP_HAT,              // C3
    KC_KP_PERC,             // C4
    KC_KP_LT,               // C5
    KC_KP_GT,               // C6
    KC_KP_AND,              // C7
    KC_KP_LAZYAND,          // C8
    KC_KP_OR,               // C9
    KC_KP_LAZYOR,           // CA
    KC_KP_COLON,            // CB
    KC_KP_HASH,             // CC
    KC_KP_SPACE,            // CD
    KC_KP_ATMARK,           // CE
    KC_KP_EXCLAMATION,      // CF
    KC_KP_MEM_STORE,        // D0
    KC_KP_MEM_RECALL,       // D1
    KC_KP_MEM_CLEAR,        // D2
    KC_KP_MEM_ADD,          // D3
    KC_KP_MEM_SUB,          // D4
    KC_KP_MEM_MUL,          // D5
    KC_KP_MEM_DIV,          // D6
    KC_KP_PLUS_MINUS,       // D7
    KC_KP_CLEAR,            // D8
    KC_KP_CLEAR_ENTRY,      // D9
    KC_KP_BINARY,           // DA
    KC_KP_OCTAL,            // DB
    KC_KP_DECIMAL,          // DC
    KC_KP_HEXADECIMAL,      // DD

    /* Modifiers */
    KC_LCTRL                = 0xE0,
    KC_LSHIFT,              // E1
    KC_LALT,                // E2
    KC_LGUI,                // E3
    KC_RCTRL,               // E4
    KC_RSHIFT,              // E5
    KC_RALT,                // E6
    KC_RGUI,                // E7
    KC_FN,                  // E8
    KC_PN,                  // E9
};

#define KEYMAP( \
    K00, K01, K02, K03, K04, K05, K06, K07, K08, K09, K0A, K0B, K0C, K0D, \
    K10, K11, K12, K13, K14, K15, K16, K17, K18, K19, K1A, K1B, K1C, K1D, \
    K20, K21, K22, K23, K24, K25, K26, K27, K28, K29, K2A, K2B,      K2D, \
    K30, K31, K32, K33, K34, K35, K36, K37, K38, K39, K3A,           K3D, \
    K40, K41, K42,                K46,                K4A, K4B, K4C, K4D  \
) { \
    KC_##K00, KC_##K01, KC_##K02, KC_##K03, KC_##K04, KC_##K05, KC_##K06, KC_##K07, KC_##K08, KC_##K09, KC_##K0A, KC_##K0B, KC_##K0C, KC_##K0D, \
    KC_##K10, KC_##K11, KC_##K12, KC_##K13, KC_##K14, KC_##K15, KC_##K16, KC_##K17, KC_##K18, KC_##K19, KC_##K1A, KC_##K1B, KC_##K1C, KC_##K1D, \
    KC_##K20, KC_##K21, KC_##K22, KC_##K23, KC_##K24, KC_##K25, KC_##K26, KC_##K27, KC_##K28, KC_##K29, KC_##K2A, KC_##K2B,           KC_##K2D, \
    KC_##K30, KC_##K31, KC_##K32, KC_##K33, KC_##K34, KC_##K35, KC_##K36, KC_##K37, KC_##K38, KC_##K39, KC_##K3A,                     KC_##K3D, \
    KC_##K40, KC_##K41, KC_##K42,                               KC_##K46,                               KC_##K4A, KC_##K4B, KC_##K4C, KC_##K4D  \
}

enum key_action {
    KA_NO,
    KA_LED,
    KA_CAPS,
    KA_MACRO,
};

#define ACTION_MAP( \
    K00, K01, K02, K03, K04, K05, K06, K07, K08, K09, K0A, K0B, K0C, K0D, \
    K10, K11, K12, K13, K14, K15, K16, K17, K18, K19, K1A, K1B, K1C, K1D, \
    K20, K21, K22, K23, K24, K25, K26, K27, K28, K29, K2A, K2B,      K2D, \
    K30, K31, K32, K33, K34, K35, K36, K37, K38, K39, K3A,           K3D, \
    K40, K41, K42,                K46,                K4A, K4B, K4C, K4D  \
) { \
    KA_##K00, KA_##K01, KA_##K02, KA_##K03, KA_##K04, KA_##K05, KA_##K06, KA_##K07, KA_##K08, KA_##K09, KA_##K0A, KA_##K0B, KA_##K0C, KA_##K0D, \
    KA_##K10, KA_##K11, KA_##K12, KA_##K13, KA_##K14, KA_##K15, KA_##K16, KA_##K17, KA_##K18, KA_##K19, KA_##K1A, KA_##K1B, KA_##K1C, KA_##K1D, \
    KA_##K20, KA_##K21, KA_##K22, KA_##K23, KA_##K24, KA_##K25, KA_##K26, KA_##K27, KA_##K28, KA_##K29, KA_##K2A, KA_##K2B,           KA_##K2D, \
    KA_##K30, KA_##K31, KA_##K32, KA_##K33, KA_##K34, KA_##K35, KA_##K36, KA_##K37, KA_##K38, KA_##K39, KA_##K3A,                     KA_##K3D, \
    KA_##K40, KA_##K41, KA_##K42,                               KA_##K46,                               KA_##K4A, KA_##K4B, KA_##K4C, KA_##K4D  \
}

struct key_info {
    uint8_t code;
    uint8_t val;
    uint8_t action;
};

int key_action_run(enum key_action action);

#endif /* __KEYBOARD_H__ */