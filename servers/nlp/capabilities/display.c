/* Copyright (C) 2026 NOS contributors. */
#include "nlp/capabilities.h"
#include <string.h>
static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t size)
{
    while (size--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
    }
    return crc;
}
static void disconnect(void *context, uint32_t session)
{
    struct nlp_display *d = context;
    if (d->active && d->owner == session) {
        d->ops.abort(d->ops.context);
        d->active = 0;
    }
}
static int call(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in, size_t size,
                uint8_t *out, size_t *used)
{
    struct nlp_display *d = context;
    if (!(b->permissions & NLP_PERMISSION_CONFIG_WRITE))
        return -NLP_DENIED;
    if (op == NLP_DISPLAY_GET_INFO && !size && *used >= 16) {
        nlp_put16(out, d->ops.width);
        nlp_put16(out + 2, d->ops.height);
        nlp_put16(out + 4, d->ops.formats);
        nlp_put16(out + 6, d->ops.modes);
        nlp_put16(out + 8, d->ops.tile_bytes);
        nlp_put16(out + 10, d->ops.max_tiles);
        nlp_put16(out + 12, d->ops.interval_ms);
        nlp_put16(out + 14, 0);
        *used = 16;
        return 0;
    }
    if (op == NLP_DISPLAY_BEGIN_FRAME && size == 8) {
        if (d->active ||
            (d->presented && b->endpoint->now_ms - d->last_frame_ms < d->ops.interval_ms))
            return -NLP_BUSY;
        if (!nlp_get32(in) || !nlp_get16(in + 4) || nlp_get16(in + 4) > d->ops.max_tiles ||
            in[6] > 1 || !(d->ops.modes & (1u << in[6])) || in[7])
            return -NLP_INVALID;
        int error = d->ops.begin(d->ops.context, in[6]);
        if (error)
            return error;
        d->active = 1;
        d->owner = b->session.id;
        d->frame = nlp_get32(in);
        d->expected = nlp_get16(in + 4);
        d->next = 0;
        d->crc = UINT32_MAX;
        *used = 0;
        return 0;
    }
    if (op == NLP_DISPLAY_TILE && size >= 18) {
        uint16_t x = nlp_get16(in + 6), y = nlp_get16(in + 8), width = nlp_get16(in + 10),
                 height = nlp_get16(in + 12);
        if (!d->active || d->owner != b->session.id || nlp_get32(in) != d->frame ||
            nlp_get16(in + 4) != d->next || d->next >= d->expected)
            return -NLP_STALE;
        if (size > d->ops.tile_bytes || in[14] > 1 || !(d->ops.formats & (1u << in[14])) ||
            in[15] || size - 18 != nlp_get16(in + 16) || !width || !height || x >= d->ops.width ||
            y >= d->ops.height || width > d->ops.width - x || height > d->ops.height - y)
            return -NLP_RANGE;
        uint32_t stride = in[14] ? ((uint32_t)width + 7) / 8 : (uint32_t)width * 2;
        if (height > (size - 18) / stride || (uint32_t)height * stride != size - 18)
            return -NLP_INVALID;
        if (in[14] && width % 8)
            for (unsigned row = 0; row < height; ++row)
                if (in[18 + row * stride + stride - 1] & ((1u << (8 - width % 8)) - 1))
                    return -NLP_INVALID;
        int error = d->ops.tile(d->ops.context, x, y, width, height, in[14], in + 18, size - 18);
        if (error)
            return error;
        d->crc = crc_update(d->crc, in, size);
        ++d->next;
        *used = 0;
        return 0;
    }
    if (op == NLP_DISPLAY_END_FRAME && size == 8) {
        if (!d->active || d->owner != b->session.id || nlp_get32(in) != d->frame ||
            d->next != d->expected || nlp_get32(in + 4) != (d->crc ^ UINT32_MAX))
            return -NLP_INTEGRITY;
        int error = d->ops.end(d->ops.context);
        if (error)
            return error;
        d->active = 0;
        d->presented = 1;
        d->last_frame_ms = b->endpoint->now_ms;
        *used = 0;
        return 0;
    }
    if (op == NLP_DISPLAY_ABORT_FRAME && size == 4) {
        if (!d->active || d->owner != b->session.id || nlp_get32(in) != d->frame)
            return -NLP_STALE;
        d->ops.abort(d->ops.context);
        d->active = 0;
        *used = 0;
        return 0;
    }
    if (op == NLP_DISPLAY_SET_POWER && size == 4 && in[0] <= 1 && !in[1] && !in[2] && !in[3]) {
        if (!d->ops.power)
            return -NLP_UNSUPPORTED;
        int error = d->ops.power(d->ops.context, in[0]);
        *used = 0;
        return error;
    }
    return -NLP_UNSUPPORTED;
}
int nlp_display_register(struct nlp_display *d, struct nlp_endpoint *e, uint16_t id,
                         const struct nlp_display_ops *ops)
{
    struct nlp_capability c = {
        NLP_UUID_DISPLAY, id, 1, 0, 1, NLP_PERMISSION_CONFIG_WRITE, 2, call, d, disconnect};
    if (!d || !e || !ops || !ops->begin || !ops->tile || !ops->end || !ops->abort || !ops->width ||
        !ops->height || !ops->formats || (ops->formats & ~3u) || !ops->modes ||
        (ops->modes & ~3u) || ops->tile_bytes < 19 || ops->tile_bytes > NLP_PAYLOAD_MAX ||
        !ops->max_tiles)
        return -NLP_INVALID;
    memset(d, 0, sizeof(*d));
    d->ops = *ops;
    return nlp_endpoint_add(e, &c);
}
