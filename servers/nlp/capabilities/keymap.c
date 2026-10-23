/* Copyright (C) 2026 NOS contributors. Atomic generation-checked canonical keymap transactions. */
#include "nlp/keymap.h"
#include "nlp/hash.h"
#include <string.h>
static void disconnect(void *context, uint32_t session)
{
    struct nlp_keymap *k = context;
    if (k->owner == session) {
        nlp_secure_clear(k->bytes, sizeof(k->bytes));
        k->active = 0;
        k->owner = 0;
    }
}
static int allowed(struct nlp_keymap *k, struct nlp_binding *b)
{
    return (b->permissions & NLP_PERMISSION_CONFIG_WRITE) && k->ops.authorized(k->ops.context, b);
}
static int valid(struct nlp_keymap *k)
{
    uint8_t digest[32];
    if (k->offset != NLP_KEYMAP_BYTES)
        return -NLP_BUSY;
    if (k->ops.generation(k->ops.context) != k->expected)
        return -NLP_CONFLICT;
    int error = nlp_sha256(k->bytes, sizeof(k->bytes), digest);
    if (error || memcmp(digest, k->digest, 32))
        return -NLP_INTEGRITY;
    error = k->ops.validate(k->ops.context, k->bytes);
    if (!error)
        k->validated = 1;
    return error;
}
static int committed(struct nlp_keymap *k, uint8_t *out, size_t *used)
{
    if (*used < 8)
        return -NLP_NO_SPACE;
    memset(out, 0, 8);
    nlp_put32(out, k->committed_generation);
    out[4] = k->state;
    *used = 8;
    return 0;
}
static int call(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in, size_t size,
                uint8_t *out, size_t *used)
{
    struct nlp_keymap *k = context;
    if (!(b->permissions & NLP_PERMISSION_DISCOVER))
        return -NLP_DENIED;
    if (op == NLP_KEYMAP_GET_LAYOUT && !size && *used >= 24) {
        memset(out, 0, 24);
        nlp_put32(out, k->ops.generation(k->ops.context));
        nlp_put32(out + 4, NLP_KEYMAP_BYTES);
        nlp_put16(out + 8, 1);
        nlp_put16(out + 12, 61);
        out[14] = 4;
        out[15] = 4;
        out[16] = 8;
        nlp_put32(out + 20, 1);
        *used = 24;
        return 0;
    }
    if (op == NLP_KEYMAP_GET_STATUS && size == 8 && *used >= 16) {
        if (nlp_get64(in) && (nlp_get64(in) != k->id || k->owner != b->session.id))
            return -NLP_STALE;
        memset(out, 0, 16);
        nlp_put64(out, k->id);
        nlp_put32(out + 8, k->ops.generation(k->ops.context));
        out[12] = (uint8_t)k->ops.persistence(k->ops.context);
        *used = 16;
        return 0;
    }
    if (op == NLP_KEYMAP_READ_PAGE) {
        if (size != 16 || nlp_get16(in + 14))
            return -NLP_INVALID;
        uint32_t offset = nlp_get32(in + 8);
        size_t length = nlp_get16(in + 12);
        if (offset > NLP_KEYMAP_BYTES || length > NLP_KEYMAP_BYTES - offset || *used < 12 ||
            length > *used - 12)
            return -NLP_RANGE;
        if (nlp_get64(in)) {
            if (!k->active || nlp_get64(in) != k->id || k->owner != b->session.id)
                return -NLP_STALE;
            if (offset + length > k->offset)
                return -NLP_RANGE;
        } else {
            if (k->active == 1)
                return -NLP_BUSY;
            int error = k->ops.snapshot(k->ops.context, k->bytes);
            if (error)
                return error;
        }
        nlp_put32(out, k->ops.generation(k->ops.context));
        nlp_put32(out + 4, offset);
        nlp_put16(out + 8, (uint16_t)length);
        nlp_put16(out + 10, 0);
        memcpy(out + 12, k->bytes + offset, length);
        *used = 12 + length;
        return 0;
    }
    if (!allowed(k, b))
        return -NLP_DENIED;
    if (op == NLP_KEYMAP_BEGIN) {
        if (size != 40 || nlp_get32(in + 4) != NLP_KEYMAP_BYTES || *used < 8)
            return -NLP_INVALID;
        if (k->active == 1)
            return -NLP_BUSY;
        if (nlp_get32(in) != k->ops.generation(k->ops.context))
            return -NLP_CONFLICT;
        if (k->next_id == UINT32_MAX)
            return -NLP_RANGE;
        k->id = ((uint64_t)b->session.id << 32) | ++k->next_id;
        k->owner = b->session.id;
        k->expected = nlp_get32(in);
        memcpy(k->digest, in + 8, 32);
        k->offset = 0;
        k->active = 1;
        k->validated = 0;
        nlp_put64(out, k->id);
        *used = 8;
        return 0;
    }
    if (size < 8 || !k->active || nlp_get64(in) != k->id || k->owner != b->session.id)
        return -NLP_STALE;
    if (op == NLP_KEYMAP_WRITE_PAGE) {
        if (size < 16 || nlp_get16(in + 14) || nlp_get16(in + 12) != size - 16 || k->active != 1 ||
            *used < 4)
            return -NLP_INVALID;
        uint32_t offset = nlp_get32(in + 8);
        size_t length = size - 16;
        if (offset > k->offset || length > NLP_KEYMAP_BYTES - offset)
            return -NLP_RANGE;
        if (offset < k->offset) {
            if (length > k->offset - offset || memcmp(k->bytes + offset, in + 16, length))
                return -NLP_CONFLICT;
        } else {
            memcpy(k->bytes + offset, in + 16, length);
            k->offset += (uint16_t)length;
            k->validated = 0;
        }
        nlp_put32(out, k->offset);
        *used = 4;
        return 0;
    }
    if (op == NLP_KEYMAP_VALIDATE && size == 8) {
        *used = 0;
        return valid(k);
    }
    if (op == NLP_KEYMAP_COMMIT) {
        if (size != 16 || nlp_get32(in + 8) != k->expected || in[12] > 1 || in[13] || in[14] ||
            in[15])
            return -NLP_INVALID;
        if (k->active == 2) {
            if (k->persist != in[12])
                return -NLP_CONFLICT;
            return committed(k, out, used);
        }
        int error = valid(k);
        if (error)
            return error;
        unsigned state = 0;
        error = k->ops.commit(k->ops.context, k->bytes, in[12], &state);
        if (error)
            return error;
        k->active = 2;
        k->persist = in[12];
        k->state = (uint8_t)state;
        k->committed_generation = k->ops.generation(k->ops.context);
        return committed(k, out, used);
    }
    if (op == NLP_KEYMAP_ABORT && size == 8) {
        if (k->active == 2)
            return -NLP_CONFLICT;
        disconnect(k, b->session.id);
        *used = 0;
        return 0;
    }
    return -NLP_UNSUPPORTED;
}
int nlp_keymap_register(struct nlp_keymap *k, struct nlp_endpoint *e, uint16_t id,
                        const struct nlp_keymap_ops *ops)
{
    struct nlp_capability c = {NLP_UUID_KEYMAP,         id, 1,    0, 1,
                               NLP_PERMISSION_DISCOVER, 1,  call, k, disconnect};
    if (!k || !e || !ops || !ops->generation || !ops->snapshot || !ops->validate || !ops->commit ||
        !ops->persistence || !ops->authorized)
        return -NLP_INVALID;
    memset(k, 0, sizeof(*k));
    k->ops = *ops;
    return nlp_endpoint_add(e, &c);
}
