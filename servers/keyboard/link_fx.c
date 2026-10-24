/* Copyright (C) 2026 NOS contributors. Atomic declarative program transactions. */
#include "service.h"
#include <string.h>
int nk60_link_program(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in,
                      size_t size, uint8_t *out, size_t *used)
{
    struct nk60_service *s = context;
    struct nk60_link *l = &s->link;
    struct kb_fx *f = &s->lighting.fx;
    if (op == NLP_LIGHTING_GET_PROGRAM_INFO && !size && *used >= 32) {
        memset(out, 0, 32);
        nlp_put16(out, 1);
        out[2] = KB_FX_RULES;
        out[3] = KB_FX_INSTANCES;
        nlp_put16(out + 4, KB_FX_PROGRAM_BYTES);
        nlp_put16(out + 6, 40);
        nlp_put16(out + 8, 1000);
        nlp_put16(out + 10, (1u << KB_FX_EFFECT_COUNT) - 1);
        nlp_put32(out + 12, (1u << KB_FX_TRIGGER_COUNT) - 1);
        nlp_put16(out + 16, (1u << KB_FX_MATCH_COUNT) - 1);
        out[18] = 15;
        for (unsigned i = 0; i < KB_FX_INSTANCES; ++i)
            if (f->instances[i].active)
                ++out[19];
        nlp_put32(out + 20, f->dropped);
        nlp_put32(out + 24, s->edits);
        out[28] = f->program[5];
        out[29] = f->program[6];
        *used = 32;
        return 0;
    }
    if (op == NLP_LIGHTING_READ_PROGRAM && size == 8) {
        unsigned offset = nlp_get16(in), count = nlp_get16(in + 2);
        if (nlp_get32(in + 4) && nlp_get32(in + 4) != s->edits)
            return -NLP_CONFLICT;
        if (!count || offset >= KB_FX_PROGRAM_BYTES || count > KB_FX_PROGRAM_BYTES - offset ||
            *used < 8 + count)
            return -NLP_RANGE;
        nlp_put32(out, s->edits);
        nlp_put16(out + 4, offset);
        nlp_put16(out + 6, count);
        memcpy(out + 8, f->program + offset, count);
        *used = 8 + count;
        return 0;
    }
    if (!(b->permissions & NLP_PERMISSION_CONFIG_WRITE) || !nk60_link_authorized(s, b))
        return -NLP_DENIED;
    if (s->saving)
        return -NLP_BUSY;
    if (s->edits == UINT32_MAX)
        return -NLP_RANGE;
    if (op == NLP_LIGHTING_BEGIN_PROGRAM) {
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
        l->frame_active = 2;
        l->program_offset = 0;
        l->frame_deadline = s->now_ms + 5000;
        memset(l->program, 0, sizeof(l->program));
        nlp_put64(out, l->frame_id);
        *used = 8;
        return 0;
    }
    if (op < NLP_LIGHTING_WRITE_PROGRAM || op > NLP_LIGHTING_ABORT_PROGRAM)
        return -NLP_UNSUPPORTED;
    if (size < 8 || l->frame_active != 2 || l->frame_owner != b->session.id ||
        nlp_get64(in) != l->frame_id)
        return -NLP_STALE;
    if (op == NLP_LIGHTING_ABORT_PROGRAM && size == 8) {
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
    if (op == NLP_LIGHTING_WRITE_PROGRAM) {
        if (size < 12)
            return -NLP_INVALID;
        unsigned offset = nlp_get16(in + 8), count = nlp_get16(in + 10);
        if (!count || size != 12 + count || offset > l->program_offset ||
            offset > KB_FX_PROGRAM_BYTES || count > KB_FX_PROGRAM_BYTES - offset)
            return -NLP_RANGE;
        if (offset < l->program_offset) {
            if (count > l->program_offset - offset || memcmp(l->program + offset, in + 12, count))
                return -NLP_CONFLICT;
        } else {
            memcpy(l->program + offset, in + 12, count);
            l->program_offset += (uint16_t)count;
        }
        *used = 0;
        return 0;
    }
    if (op == NLP_LIGHTING_COMMIT_PROGRAM && size == 16 && *used >= 8) {
        if (nlp_get32(in + 8) != s->edits)
            return -NLP_CONFLICT;
        if (in[12] > 1 || in[13] || in[14] || in[15])
            return -NLP_INVALID;
        if (l->program_offset != KB_FX_PROGRAM_BYTES)
            return -NLP_BUSY;
        if (in[12] && (!s->flash_ready || s->settings_locked))
            return -NLP_UNSUPPORTED;
        if (kb_fx_set(f, l->program, KB_FX_PROGRAM_BYTES))
            return -NLP_INVALID;
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
