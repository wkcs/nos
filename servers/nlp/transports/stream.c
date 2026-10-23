/* Copyright (C) 2026 NOS contributors. */
#include "nlp/transport.h"
#include <string.h>
int nlp_cobs_encode(const uint8_t *in, size_t length, uint8_t *out, size_t capacity,
                    size_t *written)
{
    size_t used = 1, marker = 0;
    uint8_t code = 1;
    if ((!in && length) || !out || !written || !capacity)
        return -NLP_INVALID;
    for (size_t i = 0; i < length; ++i) {
        if (used >= capacity)
            return -NLP_NO_SPACE;
        if (!in[i]) {
            out[marker] = code;
            marker = used++;
            code = 1;
        } else {
            out[used++] = in[i];
            if (++code == 255) {
                if (used >= capacity)
                    return -NLP_NO_SPACE;
                out[marker] = code;
                marker = used++;
                code = 1;
            }
        }
    }
    out[marker] = code;
    *written = used;
    return 0;
}
int nlp_cobs_decode(const uint8_t *in, size_t length, uint8_t *out, size_t capacity,
                    size_t *written)
{
    size_t at = 0, used = 0;
    if (!in || !length || !out || !written)
        return -NLP_INVALID;
    while (at < length) {
        unsigned code = in[at++], count = code ? code - 1 : 0;
        if (!code || count > length - at)
            return -NLP_INVALID;
        if (count > capacity - used)
            return -NLP_NO_SPACE;
        for (unsigned i = 0; i < count; ++i) {
            if (!in[at])
                return -NLP_INVALID;
            out[used++] = in[at++];
        }
        if (code != 255 && at < length) {
            if (used == capacity)
                return -NLP_NO_SPACE;
            out[used++] = 0;
        }
    }
    *written = used;
    return 0;
}
void nlp_stream_init(struct nlp_stream *s, unsigned mode)
{
    memset(s, 0, sizeof(*s));
    s->mode = (uint8_t)mode;
    if (mode > NLP_FRAMING_UART)
        s->failed = 1;
}
void nlp_stream_timeout(struct nlp_stream *s, uint32_t now)
{
    if (s && s->mode == NLP_FRAMING_UART && s->used && !s->ready && now - s->last_ms >= 250) {
        s->used = 0;
        s->discard = 1;
        ++s->errors;
    }
}
int nlp_stream_feed(struct nlp_stream *s, const uint8_t *in, size_t length, size_t *consumed,
                    uint32_t now, nlp_pdu_fn receive, void *context)
{
    int error;
    if (!s || (!in && length) || !consumed || !receive || length > 4096)
        return -NLP_INVALID;
    *consumed = 0;
    if (s->failed)
        return -NLP_DISCONNECTED;
    nlp_stream_timeout(s, now);
    for (;;) {
        if (s->ready) {
            error = receive(context, s->bytes + 2, s->expected);
            if (error == -NLP_BUSY || error == -NLP_NO_SPACE)
                return -NLP_BUSY;
            if (error)
                ++s->errors;
            s->ready = 0;
            s->used = 0;
            s->expected = 0;
        }
        if (*consumed == length)
            return 0;
        uint8_t byte = in[(*consumed)++];
        s->last_ms = now;
        if (s->mode == NLP_FRAMING_UART) {
            if (!byte) {
                if (!s->discard && s->used) {
                    size_t decoded = 0;
                    /* COBS can decode in place: write cursor never overtakes read. */
                    error =
                        nlp_cobs_decode(s->bytes, s->used, s->bytes, sizeof(s->bytes), &decoded);
                    if (!error && decoded >= 2) {
                        s->expected = nlp_get16(s->bytes);
                        if (s->expected >= 56 && s->expected <= NLP_PDU_MAX &&
                            decoded == 2u + s->expected)
                            s->ready = 1;
                    }
                    if (!s->ready)
                        ++s->errors;
                }
                s->discard = 0;
                if (!s->ready)
                    s->used = s->expected = 0;
            } else if (!s->discard) {
                if (s->used == sizeof(s->bytes) - 1) {
                    s->discard = 1;
                    s->used = 0;
                    ++s->errors;
                } else
                    s->bytes[s->used++] = byte;
            }
        } else {
            s->bytes[s->used++] = byte;
            if (s->used == 2) {
                s->expected = nlp_get16(s->bytes);
                if (s->expected < 56 || s->expected > NLP_PDU_MAX) {
                    s->failed = 1;
                    s->used = 0;
                    ++s->errors;
                    return -NLP_INVALID;
                }
            } else if (s->expected && s->used == 2u + s->expected)
                s->ready = 1;
        }
    }
}
int nlp_stream_wrap(unsigned mode, const uint8_t *pdu, size_t length, uint8_t *out, size_t capacity,
                    size_t *written)
{
    uint8_t raw[NLP_PDU_MAX + 2];
    int error;
    if (!pdu || !out || !written || length < 56 || length > NLP_PDU_MAX || mode > 1)
        return -NLP_INVALID;
    nlp_put16(raw, (uint16_t)length);
    memcpy(raw + 2, pdu, length);
    if (!mode) {
        if (capacity < length + 2)
            return -NLP_NO_SPACE;
        memcpy(out, raw, length + 2);
        *written = length + 2;
        return 0;
    }
    if (!capacity)
        return -NLP_NO_SPACE;
    error = nlp_cobs_encode(raw, length + 2, out, capacity - 1, written);
    if (error)
        return error;
    out[(*written)++] = 0;
    return 0;
}
