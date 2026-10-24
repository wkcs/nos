/* Copyright (C) 2026 NOS contributors. NK60 standard keymap and lighting backends. */
#include "service.h"
#include <string.h>
int nk60_link_authorized(struct nk60_service *s, const struct nlp_binding *b)
{
    return s->unlocked && s->now_ms - s->unlocked_ms < 30000 &&
           s->link.unlocked_boot == b->endpoint->peer_boot &&
           !memcmp(s->link.unlocked_uid, b->endpoint->peer_uid, 16);
}
static int authorized(void *context, const struct nlp_binding *b)
{
    return nk60_link_authorized(context, b);
}
static uint32_t generation(void *context)
{
    return ((struct nk60_service *)context)->edits;
}
static int snapshot(void *context, uint8_t out[NLP_KEYMAP_BYTES])
{
    struct nk60_service *s = context;
    return kb_config_encode(&s->keyboard.config, out, NLP_KEYMAP_BYTES) ? -NLP_INVALID : 0;
}
static int validate(void *context, const uint8_t bytes[NLP_KEYMAP_BYTES])
{
    struct nk60_service *s = context;
    return kb_config_decode(&s->candidate, bytes, NLP_KEYMAP_BYTES) ? -NLP_INVALID : 0;
}
static unsigned persistence(void *context)
{
    struct nk60_service *s = context;
    return !s->flash_ready || s->settings_locked ? 0 : s->dirty ? 1 : 2;
}
static int commit(void *context, const uint8_t bytes[NLP_KEYMAP_BYTES], int persist,
                  unsigned *state)
{
    struct nk60_service *s = context;
    int error = nk60_service_commit_wire(s, bytes, persist);
    if (!error)
        *state = persist ? 1 : 0;
    return error;
}
void nk60_light_disconnect(void *context, uint32_t session)
{
    struct nk60_service *s = context;
    if (s->link.frame_active && s->link.frame_owner == session) {
        memset(s->link.program, 0, sizeof(s->link.program));
        s->link.frame_active = 0;
        s->link.frame_received = 0;
    }
}
void nk60_light_poll(struct nk60_service *s)
{
    if (s->link.frame_active && ((int32_t)(s->now_ms - s->link.frame_deadline) >= 0 ||
                                 !s->unlocked || s->now_ms - s->unlocked_ms >= 30000))
        nk60_light_disconnect(s, s->link.frame_owner);
}
static int lighting(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in,
                    size_t size, uint8_t *out, size_t *used)
{
    if (op >= NLP_LIGHTING_GET_PROGRAM_INFO)
        return nk60_link_program(context, b, op, in, size, out, used);
    struct nk60_service *s = context;
    struct nk60_link *l = &s->link;
    struct kb_light_settings *p = &s->lighting.settings;
    if (op == NLP_LIGHTING_GET_INFO && !size && *used >= 8) {
        nlp_put16(out, KB_KEYS);
        nlp_put16(out + 2, 1000 / kb_fx_interval(&s->lighting.fx));
        out[4] = 255;
        out[5] = KB_LIGHT_MODE_MASK;
        nlp_put16(out + 6, 63);
        *used = 8;
        return 0;
    }
    if (op == NLP_LIGHTING_GET_STATE && !size && *used >= 11) {
        nlp_put32(out, s->edits);
        out[4] = s->keyboard.config.rgb_mode;
        out[5] = s->keyboard.config.brightness;
        nlp_put16(out + 6, 0);
        out[8] = p->color.r;
        out[9] = p->color.g;
        out[10] = p->color.b;
        *used = 11;
        return 0;
    }
    if (op == NLP_LIGHTING_GET_SETTINGS && !size && *used >= 16) {
        memset(out, 0, 16);
        nlp_put32(out, s->edits);
        out[4] = p->speed;
        out[5] = p->flags;
        nlp_put16(out + 6, p->idle_seconds);
        nlp_put16(out + 8, p->current_ma);
        out[10] = p->color.r;
        out[11] = p->color.g;
        out[12] = p->color.b;
        out[13] = s->settings_locked || !s->flash_ready ? 0 : s->dirty ? 1 : 2;
        *used = 16;
        return 0;
    }
    if (op == NLP_LIGHTING_READ_LEDS && size == 8) {
        unsigned first = nlp_get16(in), count = nlp_get16(in + 2);
        if (nlp_get32(in + 4) && nlp_get32(in + 4) != s->edits)
            return -NLP_CONFLICT;
        if (!count || first >= KB_KEYS || count > KB_KEYS - first || *used < 8 + count * 3)
            return -NLP_RANGE;
        nlp_put32(out, s->edits);
        nlp_put16(out + 4, first);
        nlp_put16(out + 6, count);
        for (unsigned i = 0; i < count; ++i) {
            out[8 + i * 3] = p->custom[first + i].r;
            out[9 + i * 3] = p->custom[first + i].g;
            out[10 + i * 3] = p->custom[first + i].b;
        }
        *used = 8 + count * 3;
        return 0;
    }
    if (!(b->permissions & NLP_PERMISSION_CONFIG_WRITE) || !nk60_link_authorized(s, b))
        return -NLP_DENIED;
    if (s->saving)
        return -NLP_BUSY;
    if (s->edits == UINT32_MAX)
        return -NLP_RANGE;
    if (op == NLP_LIGHTING_SET) {
        if ((size != 11 && size != 12) || (size == 12 && in[11] > 1) || in[4] >= KB_LIGHT_MODES ||
            nlp_get16(in + 6))
            return -NLP_INVALID;
        if (nlp_get32(in) != s->edits)
            return -NLP_CONFLICT;
        if (size == 12 && in[11] && (!s->flash_ready || s->settings_locked))
            return -NLP_UNSUPPORTED;
        s->keyboard.config.rgb_mode = in[4];
        s->keyboard.config.brightness = in[5];
        p->color = (struct nk60_rgb){in[8], in[9], in[10]};
        nk60_service_changed(s);
        if (size == 12 && in[11])
            s->save_requested = 1;
        *used = 0;
        return 0;
    }
    if (op == NLP_LIGHTING_SET_SETTINGS) {
        if (size != 16 || !in[4] || (in[5] & ~KB_LIGHT_INDICATORS) || nlp_get16(in + 6) > 3600 ||
            nlp_get16(in + 8) < 100 || nlp_get16(in + 8) > 350 || in[13] > 1 || nlp_get16(in + 14))
            return -NLP_INVALID;
        if (nlp_get32(in) != s->edits)
            return -NLP_CONFLICT;
        if (in[13] && (!s->flash_ready || s->settings_locked))
            return -NLP_UNSUPPORTED;
        p->speed = in[4];
        p->flags = in[5];
        p->idle_seconds = nlp_get16(in + 6);
        p->current_ma = nlp_get16(in + 8);
        p->color = (struct nk60_rgb){in[10], in[11], in[12]};
        nk60_service_changed(s);
        if (in[13])
            s->save_requested = 1;
        *used = 0;
        return 0;
    }
    if (op == NLP_LIGHTING_BEGIN_FRAME) {
        if (size != 4 || *used < 8)
            return -NLP_INVALID;
        if (nlp_get32(in) != s->edits)
            return -NLP_CONFLICT;
        if (l->frame_active)
            return -NLP_BUSY;
        if (l->frame_next == UINT32_MAX)
            return -NLP_RANGE;
        l->frame_id = ((uint64_t)b->session.id << 32) | ++l->frame_next;
        l->frame_owner = b->session.id;
        l->frame_generation = s->edits;
        l->frame_active = 1;
        l->frame_received = 0;
        l->frame_deadline = s->now_ms + 5000;
        nlp_put64(out, l->frame_id);
        *used = 8;
        return 0;
    }
    if (op < NLP_LIGHTING_WRITE_FRAME || op > NLP_LIGHTING_ABORT_FRAME)
        return -NLP_UNSUPPORTED;
    if (size < 8 || l->frame_active != 1 || l->frame_owner != b->session.id ||
        nlp_get64(in) != l->frame_id)
        return -NLP_STALE;
    if (op == NLP_LIGHTING_ABORT_FRAME && size == 8) {
        nk60_light_disconnect(s, b->session.id);
        *used = 0;
        return 0;
    }
    if ((int32_t)(s->now_ms - l->frame_deadline) >= 0) {
        nk60_light_disconnect(s, b->session.id);
        return -NLP_STALE;
    }
    if (l->frame_generation != s->edits)
        return -NLP_CONFLICT;
    if (op == NLP_LIGHTING_WRITE_FRAME) {
        if (size < 12)
            return -NLP_INVALID;
        unsigned first = nlp_get16(in + 8), count = nlp_get16(in + 10);
        if (!count || first >= KB_KEYS || count > KB_KEYS - first || size != 12 + count * 3)
            return -NLP_RANGE;
        for (unsigned i = 0; i < count; ++i) {
            unsigned key = first + i;
            if ((l->frame_received & (UINT64_C(1) << key)) &&
                (l->frame[key].r != in[12 + i * 3] || l->frame[key].g != in[13 + i * 3] ||
                 l->frame[key].b != in[14 + i * 3]))
                return -NLP_CONFLICT;
        }
        for (unsigned i = 0; i < count; ++i) {
            l->frame[first + i] = (struct nk60_rgb){in[12 + i * 3], in[13 + i * 3], in[14 + i * 3]};
            l->frame_received |= UINT64_C(1) << (first + i);
        }
        *used = 0;
        return 0;
    }
    if (op == NLP_LIGHTING_COMMIT_FRAME && size == 16 && *used >= 8) {
        if (nlp_get32(in + 8) != s->edits)
            return -NLP_CONFLICT;
        if (in[12] > 1 || in[13] || in[14] || in[15])
            return -NLP_INVALID;
        if (l->frame_received != (UINT64_C(1) << KB_KEYS) - 1)
            return -NLP_BUSY;
        if (in[12] && (!s->flash_ready || s->settings_locked))
            return -NLP_UNSUPPORTED;
        memcpy(p->custom, l->frame, sizeof(p->custom));
        s->keyboard.config.rgb_mode = KB_LIGHT_CUSTOM;
        nk60_service_changed(s);
        if (in[12])
            s->save_requested = 1;
        memset(out, 0, 8);
        nlp_put32(out, s->edits);
        out[4] = in[12] ? 1 : 0;
        nk60_light_disconnect(s, b->session.id);
        *used = 8;
        return 0;
    }
    return -NLP_INVALID;
}
int nk60_link_capabilities(struct nk60_service *s)
{
    const struct nlp_keymap_ops ops = {s,      generation,  snapshot,  validate,
                                       commit, persistence, authorized};
    struct nlp_capability light = {NLP_UUID_LIGHTING,       4, 1,        2, 1,
                                   NLP_PERMISSION_DISCOVER, 1, lighting, s, nk60_light_disconnect};
    int error = nlp_keymap_register(&s->link.keymap, &s->link.endpoint, 3, &ops);
    if (!error && s->rgb_ready)
        error = nlp_endpoint_add(&s->link.endpoint, &light);
    if (!error)
        error = nk60_link_device_register(s);
    return error;
}
