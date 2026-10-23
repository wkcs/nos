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
    return s->dirty ? 1 : s->flash_ready ? 2 : 0;
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
static int lighting(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in,
                    size_t size, uint8_t *out, size_t *used)
{
    struct nk60_service *s = context;
    if (op == NLP_LIGHTING_GET_INFO && !size && *used >= 8) {
        nlp_put16(out, 61);
        nlp_put16(out + 2, 10);
        out[4] = 255;
        out[5] = 7;
        nlp_put16(out + 6, 3);
        *used = 8;
        return 0;
    }
    if (op == NLP_LIGHTING_GET_STATE && !size && *used >= 11) {
        nlp_put32(out, s->edits);
        out[4] = s->keyboard.config.rgb_mode;
        out[5] = s->keyboard.config.brightness;
        nlp_put16(out + 6, 0);
        memcpy(out + 8, s->link.static_rgb, 3);
        *used = 11;
        return 0;
    }
    if (op != NLP_LIGHTING_SET)
        return -NLP_UNSUPPORTED;
    if (!(b->permissions & NLP_PERMISSION_CONFIG_WRITE) || !nk60_link_authorized(s, b))
        return -NLP_DENIED;
    if (size != 11 || in[4] > 2 || nlp_get16(in + 6))
        return -NLP_INVALID;
    if (nlp_get32(in) != s->edits)
        return -NLP_CONFLICT;
    if (s->saving)
        return -NLP_BUSY;
    if (s->edits == UINT32_MAX)
        return -NLP_RANGE;
    s->keyboard.config.rgb_mode = in[4];
    s->keyboard.config.brightness = in[5];
    memcpy(s->link.static_rgb, in + 8, 3);
    s->dirty = s->rgb_pending = 1;
    s->last_edit_ms = s->now_ms;
    ++s->edits;
    *used = 0;
    return 0;
}
int nk60_link_capabilities(struct nk60_service *s)
{
    const struct nlp_keymap_ops ops = {s,      generation,  snapshot,  validate,
                                       commit, persistence, authorized};
    struct nlp_capability light = {NLP_UUID_LIGHTING,       4, 1,        0, 1,
                                   NLP_PERMISSION_DISCOVER, 1, lighting, s, NULL};
    int error = nlp_keymap_register(&s->link.keymap, &s->link.endpoint, 3, &ops);
    if (!error && s->rgb_ready)
        error = nlp_endpoint_add(&s->link.endpoint, &light);
    return error;
}
