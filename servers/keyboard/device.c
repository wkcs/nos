/* Copyright (C) 2026 NOS contributors. Product metadata; no raw memory/flash access. */
#include "service.h"
#include "layout.h"
#include <string.h>
static int call(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in, size_t size,
                uint8_t *out, size_t *used)
{
    struct nk60_service *s = context;
    if (!(b->permissions & NLP_PERMISSION_DISCOVER))
        return -NLP_DENIED;
    if (op == NLP_KEYBOARD_CONTROL_GET_INFO && !size && *used >= 32) {
        memset(out, 0, 32);
        nlp_put16(out, 1);
        nlp_put16(out + 2, KB_KEYS);
        nlp_put16(out + 4, NK60_LED_COUNT);
        nlp_put16(out + 6, KB_LAYOUT_WIDTH);
        nlp_put16(out + 8, KB_LAYOUT_HEIGHT);
        out[10] = 4;
        out[11] = KB_LAYERS;
        nlp_put32(out + 12, NK60_SERVICE_SCAN_US);
        nlp_put32(out + 16, s->hardware.flash_bytes);
        /* Actual: layout, diagnostics, NKRO, keymap, per-key RGB, persistence. */
        nlp_put32(out + 20, 207u | (s->rgb_ready ? 16u : 0u) | (s->flash_ready ? 32u : 0u));
        /* Reserved extension groups are explicitly NOT active capabilities. */
        nlp_put32(out + 24, 127u);
        nlp_put32(out + 28, s->edits);
        *used = 32;
        return 0;
    }
    if (op == NLP_KEYBOARD_CONTROL_GET_LAYOUT && size == 8) {
        unsigned first = nlp_get16(in), maximum = nlp_get16(in + 2);
        if (nlp_get32(in + 4) || first >= KB_KEYS || !maximum || *used < 20)
            return -NLP_INVALID;
        unsigned count = (*used - 8) / 12;
        if (count > maximum)
            count = maximum;
        if (count > KB_KEYS - first)
            count = KB_KEYS - first;
        nlp_put16(out, first);
        nlp_put16(out + 2, count);
        nlp_put16(out + 4, first + count == KB_KEYS ? 0 : first + count);
        nlp_put16(out + 6, KB_KEYS);
        for (unsigned i = 0; i < count; ++i) {
            struct kb_key_position p;
            kb_layout_position(first + i, &p);
            uint8_t *entry = out + 8 + i * 12;
            nlp_put16(entry, first + i);
            nlp_put16(entry + 2, p.led);
            nlp_put16(entry + 4, p.x);
            nlp_put16(entry + 6, p.y);
            nlp_put16(entry + 8, p.width);
            nlp_put16(entry + 10, p.height);
        }
        *used = 8 + count * 12;
        return 0;
    }
    if (op == NLP_KEYBOARD_CONTROL_GET_STORAGE && !size && *used >= 56) {
        memset(out, 0, 56);
        uint32_t capacity = s->flash_ready ? s->hardware.flash_bytes : 0;
        nlp_put32(out, capacity);
        nlp_put16(out + 4, NK60_FLASH_SECTOR_SIZE);
        nlp_put16(out + 6, NK60_FLASH_PAGE_SIZE);
        if (capacity >= 16384) {
            out[8] = 3;
            /* 12-byte entries: kind:u16, flags:u16, offset:u32, bytes:u32.
             * flags: bit0 owned by NOS, bit1 atomic journal, bit2 preserved/unallocated. */
            nlp_put16(out + 12, 0);
            nlp_put16(out + 14, 4);
            nlp_put32(out + 20, capacity - 16384);
            nlp_put16(out + 24, 1);
            nlp_put16(out + 26, 3);
            nlp_put32(out + 28, capacity - 16384);
            nlp_put32(out + 32, 8192);
            nlp_put16(out + 36, 2);
            nlp_put16(out + 38, 3);
            nlp_put32(out + 40, capacity - 8192);
            nlp_put32(out + 44, 8192);
        }
        nlp_put32(out + 48, s->generation);
        out[52] = s->dirty;
        out[53] = s->settings_locked;
        out[54] = s->saving;
        *used = 56;
        return 0;
    }
    if (op == NLP_KEYBOARD_CONTROL_GET_DIAGNOSTICS && !size && *used >= 48) {
        memset(out, 0, 48);
        nlp_put64(out, s->monotonic_us);
        nlp_put32(out + 8, s->scan_overruns);
        nlp_put32(out + 12, s->scan_errors);
        nlp_put32(out + 16, s->keyboard.overflows);
        nlp_put32(out + 20, s->usb_errors);
        nlp_put32(out + 24, s->save_errors);
        nlp_put32(out + 28, s->rgb_errors);
        nlp_put32(out + 32, s->link.endpoint.bad_frames);
        nlp_put32(out + 36, s->tx_dropped);
        nlp_put16(out + 40, s->lighting.estimated_ma);
        out[42] = s->configured;
        out[43] = s->suspended;
        out[44] = s->settings_locked;
        out[45] = s->unlocked;
        out[46] = KB_LIGHT_CHANNEL_LIMIT;
        out[47] = s->flash_ready;
        *used = 48;
        return 0;
    }
    return -NLP_UNSUPPORTED;
}
int nk60_link_device_register(struct nk60_service *s)
{
    const struct nlp_capability c = {NLP_UUID_KEYBOARD_CONTROL, 5, 1,    0, 1,
                                     NLP_PERMISSION_DISCOVER,   1, call, s, NULL};
    return nlp_endpoint_add(&s->link.endpoint, &c);
}
