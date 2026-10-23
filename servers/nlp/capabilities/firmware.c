/* Copyright (C) 2026 NOS contributors. Bounded manifest, idempotent writes, durable checkpoints. */
#include "nlp/firmware.h"
#include <string.h>
static int manifest(struct nlp_firmware *f)
{
    struct nlp_update *u = &f->update;
    struct nlp_tlv_reader reader = {u->manifest, u->manifest_size, 0, 0, 0};
    struct nlp_tlv t;
    uint8_t digest[32], canonical[519];
    size_t used = 7;
    uint16_t fields = 0;
    uint32_t key = 0, imagesize = 0;
    const uint8_t *signature = NULL, *imagehash = NULL;
    int next;
    memcpy(canonical, "NLP-FW1", 7);
    int error = f->ops.crypto->hash(u->manifest, u->manifest_size, digest);
    if (error || memcmp(digest, u->digest, 32))
        return -NLP_INTEGRITY;
    while ((next = nlp_tlv_next(&reader, &t)) > 0) {
        unsigned tag = t.tag & 0x7fff;
        if (tag <= 10)
            fields |= (uint16_t)(1u << tag);
        switch (tag) {
        case 1:
            if (t.length != 16 || memcmp(t.value, f->ops.board, 16))
                return -NLP_CONFLICT;
            break;
        case 2:
            if (t.length != 4 || nlp_get32(t.value) > f->ops.hardware)
                return -NLP_CONFLICT;
            break;
        case 3:
            if (t.length != 4 || nlp_get32(t.value) < f->ops.hardware)
                return -NLP_CONFLICT;
            break;
        case 4:
            if (t.length != 16 || memcmp(t.value, f->ops.component, 16))
                return -NLP_CONFLICT;
            break;
        case 5:
            if (t.length != 4 || !(imagesize = nlp_get32(t.value)) ||
                imagesize > f->ops.image_limit)
                return -NLP_RANGE;
            break;
        case 6:
            if (t.length != 32)
                return -NLP_INVALID;
            imagehash = t.value;
            break;
        case 7:
            if (t.length != 8)
                return -NLP_INVALID;
            break;
        case 8:
            if (t.length != 8 || nlp_get64(t.value) < f->ops.rollback_index)
                return -NLP_DENIED;
            break;
        case 9:
            if (t.length != 4)
                return -NLP_INVALID;
            key = nlp_get32(t.value);
            break;
        case 10:
            if (t.length != 64)
                return -NLP_INVALID;
            signature = t.value;
            break;
        default:
            if (t.tag & 0x8000)
                return -NLP_UNSUPPORTED;
            break;
        }
        if (tag != 10) {
            if (used + 4u + t.length > sizeof(canonical))
                return -NLP_NO_SPACE;
            memcpy(canonical + used, t.value - 4, t.length + 4u);
            used += 4u + t.length;
        }
    }
    if (next < 0 || fields != 0x7fe || !signature || !imagehash)
        return -NLP_INVALID;
    error = f->ops.signature(f->ops.context, key, canonical, used, signature);
    if (error)
        return error;
    u->image_size = imagesize;
    memcpy(u->image_digest, imagehash, 32);
    f->valid = 1;
    return 0;
}
static int status(struct nlp_firmware *f, uint8_t *out, size_t *used)
{
    if (*used < 24)
        return -NLP_NO_SPACE;
    struct nlp_update *u = &f->update;
    memset(out, 0, 24);
    nlp_put64(out, u->id);
    nlp_put16(out + 8, NLP_PAYLOAD_MAX - 24);
    out[10] = u->state;
    nlp_put16(out + 12, u->manifest_offset);
    nlp_put32(out + 16, u->accepted);
    nlp_put32(out + 20, u->durable);
    *used = 24;
    return 0;
}
static int call(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in, size_t size,
                uint8_t *out, size_t *used)
{
    struct nlp_firmware *f = context;
    struct nlp_update *u = &f->update;
    int error;
    if (!(b->permissions & NLP_PERMISSION_FIRMWARE_STAGE))
        return -NLP_DENIED;
    if (op == NLP_FIRMWARE_GET_TARGETS && !size) {
        if (*used < 64)
            return -NLP_MTU_TOO_SMALL;
        memcpy(out, f->ops.component, 16);
        memcpy(out + 16, f->ops.board, 16);
        nlp_put32(out + 32, f->ops.hardware);
        nlp_put32(out + 36, f->ops.image_limit);
        nlp_put32(out + 40, f->ops.erase_granule);
        nlp_put16(out + 44, b->session.payload_limit - 24);
        out[46] = f->ops.mode;
        out[47] = f->ops.apply ? 1 : 0;
        nlp_put64(out + 48, f->ops.current_version);
        nlp_put64(out + 56, f->ops.rollback_index);
        *used = 64;
        return 0;
    }
    if (op == NLP_FIRMWARE_BEGIN) {
        if (size != 52 || memcmp(in, f->ops.component, 16) || !nlp_get16(in + 16) ||
            nlp_get16(in + 16) > 512 || nlp_get16(in + 18) != 1)
            return -NLP_INVALID;
        if (u->state) {
            if (memcmp(in + 20, u->digest, 32) || nlp_get16(in + 16) != u->manifest_size)
                return -NLP_BUSY;
            return status(f, out, used);
        }
        uint64_t id = 0;
        error = f->ops.allocate_id(f->ops.context, &id);
        if (error)
            return error;
        if (!id)
            return -NLP_INTEGRITY;
        memset(u, 0, sizeof(*u));
        u->id = id;
        u->state = NLP_UPDATE_MANIFEST;
        u->manifest_size = nlp_get16(in + 16);
        memcpy(u->digest, in + 20, 32);
        error = f->ops.checkpoint(f->ops.context, u);
        if (error) {
            memset(u, 0, sizeof(*u));
            return error;
        }
        return status(f, out, used);
    }
    if (size < 8 || nlp_get64(in) != u->id || !u->state)
        return -NLP_STALE;
    if (op == NLP_FIRMWARE_QUERY && size == 8)
        return status(f, out, used);
    if (op == NLP_FIRMWARE_MANIFEST_CHUNK || op == NLP_FIRMWARE_WRITE) {
        if (size < 16 || nlp_get16(in + 14) || size - 16 != nlp_get16(in + 12) || size == 16)
            return -NLP_INVALID;
        uint32_t offset = nlp_get32(in + 8);
        size_t count = size - 16;
        if (op == NLP_FIRMWARE_MANIFEST_CHUNK) {
            if (u->state != NLP_UPDATE_MANIFEST || offset > u->manifest_offset ||
                offset > u->manifest_size || count > u->manifest_size - offset)
                return -NLP_RANGE;
            if (offset < u->manifest_offset) {
                if (count > u->manifest_offset - offset ||
                    memcmp(in + 16, u->manifest + offset, count))
                    return -NLP_CONFLICT;
            } else {
                memcpy(u->manifest + offset, in + 16, count);
                u->manifest_offset += (uint16_t)count;
            }
            if (u->manifest_offset == u->manifest_size) {
                error = manifest(f);
                if (error) {
                    u->manifest_offset = 0;
                    return error;
                }
                error = f->ops.prepare(f->ops.context, u->image_size);
                if (error)
                    return error;
                u->state = NLP_UPDATE_RECEIVING;
                error = f->ops.checkpoint(f->ops.context, u);
                if (error) {
                    u->state = NLP_UPDATE_MANIFEST;
                    return error;
                }
            }
            return status(f, out, used);
        }
        if (u->state != NLP_UPDATE_RECEIVING || !f->valid || offset > u->accepted ||
            offset > u->image_size || count > u->image_size - offset)
            return -NLP_RANGE;
        if (offset < u->accepted) {
            uint8_t previous[NLP_PAYLOAD_MAX];
            if (count > sizeof(previous) || count > u->accepted - offset)
                return -NLP_CONFLICT;
            error = f->ops.read(f->ops.context, offset, previous, count);
            if (error)
                return error;
            if (memcmp(previous, in + 16, count))
                return -NLP_CONFLICT;
        } else {
            if (count > f->ops.erase_granule - offset % f->ops.erase_granule)
                return -NLP_RANGE;
            error = f->ops.write(f->ops.context, offset, in + 16, count);
            if (error)
                return error;
            u->accepted += (uint32_t)count;
            if (!(u->accepted % f->ops.erase_granule) || u->accepted == u->image_size) {
                uint32_t old = u->durable;
                u->durable = u->accepted;
                error = f->ops.checkpoint(f->ops.context, u);
                if (error) {
                    u->durable = old;
                    return error;
                }
            }
        }
        if (*used < 8)
            return -NLP_NO_SPACE;
        nlp_put32(out, u->accepted);
        nlp_put32(out + 4, u->durable);
        *used = 8;
        return 0;
    }
    if (size != 8)
        return -NLP_INVALID;
    if (op == NLP_FIRMWARE_VERIFY) {
        if (u->accepted != u->image_size || u->durable != u->image_size ||
            u->state != NLP_UPDATE_RECEIVING)
            return -NLP_BUSY;
        error = manifest(f);
        if (!error)
            error = f->ops.verify_image(f->ops.context, u->image_size, u->image_digest);
        if (error)
            return error;
        u->state = NLP_UPDATE_VERIFIED;
        error = f->ops.checkpoint(f->ops.context, u);
        if (error) {
            u->state = NLP_UPDATE_RECEIVING;
            return error;
        }
        *used = 0;
        return 0;
    }
    if (op == NLP_FIRMWARE_APPLY || op == NLP_FIRMWARE_CONFIRM) {
        if (!(b->permissions & NLP_PERMISSION_FIRMWARE_APPLY))
            return -NLP_DENIED;
        if (!f->ops.apply || !f->ops.mode)
            return -NLP_UNSUPPORTED;
        if (op == NLP_FIRMWARE_APPLY) {
            if (u->state == NLP_UPDATE_COMMITTED) {
                *used = 0;
                return 0;
            }
            if (u->state != NLP_UPDATE_VERIFIED && u->state != NLP_UPDATE_PENDING)
                return -NLP_BUSY;
            error = manifest(f);
            if (error)
                return error;
            error = f->ops.verify_image(f->ops.context, u->image_size, u->image_digest);
            if (error)
                return error;
            uint8_t old_state = u->state;
            u->state = NLP_UPDATE_PENDING;
            error = f->ops.checkpoint(f->ops.context, u);
            if (error)
                u->state = old_state;
            if (!error)
                error = f->ops.apply(f->ops.context, u->id);
        } else {
            if (!f->ops.confirm)
                return -NLP_UNSUPPORTED;
            if (u->state == NLP_UPDATE_COMMITTED) {
                *used = 0;
                return 0;
            }
            if (u->state != NLP_UPDATE_PENDING)
                return -NLP_BUSY;
            error = f->ops.confirm(f->ops.context, u->id);
            if (!error) {
                u->state = NLP_UPDATE_COMMITTED;
                error = f->ops.checkpoint(f->ops.context, u);
            }
        }
        *used = 0;
        return error;
    }
    if (op == NLP_FIRMWARE_ABORT) {
        if (u->state >= NLP_UPDATE_PENDING)
            return -NLP_CONFLICT;
        struct nlp_update empty = {0};
        uint64_t old_id = u->id;
        error = f->ops.checkpoint(f->ops.context, &empty);
        if (error)
            return error;
        memset(u, 0, sizeof(*u));
        f->valid = 0;
        *used = 0;
        return f->ops.abort(f->ops.context, old_id);
    }
    return -NLP_UNSUPPORTED;
}
int nlp_firmware_register(struct nlp_firmware *f, struct nlp_endpoint *e, uint16_t id,
                          const struct nlp_firmware_ops *ops)
{
    struct nlp_capability c = {
        NLP_UUID_FIRMWARE, id, 1, 0, 1, NLP_PERMISSION_FIRMWARE_STAGE, 1, call, f, NULL};
    if (!f || !e || !ops || !ops->crypto || !ops->crypto->hash || !ops->allocate_id || !ops->load ||
        !ops->checkpoint || !ops->signature || !ops->prepare || !ops->read || !ops->write ||
        !ops->recover || !ops->verify_image || !ops->abort || !ops->image_limit ||
        !ops->erase_granule || ops->erase_granule > ops->image_limit || ops->mode > 2 ||
        (ops->apply && !ops->mode))
        return -NLP_INVALID;
    memset(f, 0, sizeof(*f));
    f->ops = *ops;
    int error = ops->load(ops->context, &f->update);
    if (error && error != -NLP_NOT_FOUND)
        return error;
    if (error)
        memset(&f->update, 0, sizeof(f->update));
    struct nlp_update *u = &f->update;
    if (u->state > NLP_UPDATE_COMMITTED ||
        (u->state && (!u->id || !u->manifest_size || u->manifest_size > 512 ||
                      u->manifest_offset > u->manifest_size || u->accepted > ops->image_limit ||
                      u->durable > u->accepted)))
        return -NLP_INTEGRITY;
    if (u->state == NLP_UPDATE_MANIFEST)
        u->manifest_offset = 0;
    if (u->state >= NLP_UPDATE_RECEIVING) {
        if (u->manifest_offset != u->manifest_size)
            return -NLP_INTEGRITY;
        error = manifest(f);
        if (error)
            return error;
        if (u->accepted > u->image_size || u->durable > u->image_size)
            return -NLP_INTEGRITY;
        u->accepted = u->durable;
        if (u->state == NLP_UPDATE_RECEIVING && (error = ops->recover(ops->context, u->durable)))
            return error;
    }
    return nlp_endpoint_add(e, &c);
}
