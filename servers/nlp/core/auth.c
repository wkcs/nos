/* Copyright (C) 2026 NOS contributors. Noise authenticated admission and per-capability keys. */
#include "nlp/auth.h"
#include <string.h>
int nlp_auth_init(struct nlp_auth *a, const struct nlp_crypto *crypto, const uint8_t key[32],
                  const uint8_t peer_pin[32], uint8_t uid[16])
{
    uint8_t public_key[32], digest[32];
    if (!a || !crypto || !key || !peer_pin || !uid || !crypto->public_key || !crypto->hash)
        return -NLP_INVALID;
    memset(a, 0, sizeof(*a));
    a->crypto = crypto;
    memcpy(a->static_key, key, 32);
    memcpy(a->peer_pin, peer_pin, 32);
    int error = crypto->public_key(key, public_key);
    if (!error)
        error = crypto->hash(public_key, 32, digest);
    if (!error)
        memcpy(uid, digest, 16);
    nlp_secure_clear(digest, 32);
    return error;
}
void nlp_auth_reset(struct nlp_auth *a)
{
    if (!a)
        return;
    nlp_secure_clear(&a->noise, sizeof(a->noise));
    nlp_secure_clear(a->cipher, sizeof(a->cipher));
    a->request_size = a->request_opcode = a->reply_size = 0;
    a->request_id = a->session_id = 0;
}
static int verify(void *context, const uint8_t key[32])
{
    struct nlp_auth *a = context;
    uint8_t difference = 0;
    for (unsigned i = 0; i < 32; ++i)
        difference |= key[i] ^ a->peer_pin[i];
    return difference ? -NLP_DENIED : 0;
}
static size_t prologue(struct nlp_endpoint *e, uint32_t id, uint8_t out[92])
{
    int lower = memcmp(e->config.uid, e->peer_uid, 16) < 0;
    memcpy(out, "NLP1AUTH", 8);
    nlp_put16(out + 8, 1);
    nlp_put16(out + 10, e->peer_payload);
    nlp_put16(out + 12, 2);
    nlp_put16(out + 14, 1);
    memcpy(out + 16, e->config.fabric, 16);
    nlp_put32(out + 32, e->config.epoch);
    nlp_put32(out + 36, e->peer_nonce);
    nlp_put32(out + 40, id);
    memcpy(out + 44, lower ? e->config.uid : e->peer_uid, 16);
    memcpy(out + 60, lower ? e->peer_uid : e->config.uid, 16);
    nlp_put64(out + 76, lower ? e->config.boot_id : e->peer_boot);
    nlp_put64(out + 84, lower ? e->peer_boot : e->config.boot_id);
    return 92;
}
static int reply(struct nlp_endpoint *e, const struct nlp_frame *f, unsigned opcode, unsigned stage,
                 const uint8_t *message, size_t size)
{
    struct nlp_auth *a = e->config.auth;
    struct nlp_header h;
    uint8_t payload[NLP_PAYLOAD_MAX], value[4];
    size_t used = 8, written;
    memset(&h, 0, sizeof(h));
    h.kind = NLP_CONTROL;
    h.priority = 0;
    h.hop_limit = 1;
    h.src_node = e->config.node;
    h.dst_node = f->h.src_node;
    h.fabric_epoch = e->config.epoch;
    h.opcode = (uint16_t)opcode;
    memset(payload, 0, 8);
    nlp_put32(payload, nlp_get32(f->payload));
    value[0] = (uint8_t)stage;
    int error = nlp_tlv_add(payload, sizeof(payload), &used, 15, value, 1);
    if (!error)
        error = nlp_tlv_add(payload, sizeof(payload), &used, 16, message, size);
    nlp_put32(value, a->session_id);
    if (!error)
        error = nlp_tlv_add(payload, sizeof(payload), &used, 17, value, 4);
    if (!error)
        error = nlp_frame_encode(&h, payload, used, a->reply, sizeof(a->reply), &written);
    if (error)
        return error;
    a->reply_size = (uint16_t)written;
    return e->config.send(e->config.context, a->reply, a->reply_size);
}
int nlp_auth_control(struct nlp_endpoint *e, const struct nlp_frame *f)
{
    struct nlp_auth *a = e->config.auth;
    struct nlp_tlv_reader r = {f->payload + 8, f->length - 8u, 0, 0, 0};
    struct nlp_tlv t;
    const uint8_t *message = NULL;
    uint8_t stage = 0, digest[32], bytes[96], p[92];
    uint32_t id = 0, request = nlp_get32(f->payload);
    size_t size = 0, written;
    int error, next;
    if (!a || e->joined || f->length > NLP_PAYLOAD_MAX || !e->seen)
        return -NLP_DENIED;
    if (request == a->request_id && a->request_size) {
        if (f->length != a->request_size || f->h.opcode != a->request_opcode ||
            memcmp(f->payload, a->request, f->length))
            return -NLP_CONFLICT;
        return a->reply_size ? e->config.send(e->config.context, a->reply, a->reply_size) : 0;
    }
    if (request <= a->request_id)
        return -NLP_STALE;
    if (a->noise.complete)
        return -NLP_CONFLICT;
    while ((next = nlp_tlv_next(&r, &t)) > 0) {
        switch (t.tag & 0x7fff) {
        case 15:
            if (t.length != 1)
                return -NLP_INVALID;
            stage = t.value[0];
            break;
        case 16:
            if (t.length > 96)
                return -NLP_INVALID;
            message = t.value;
            size = t.length;
            break;
        case 17:
            if (t.length != 4)
                return -NLP_INVALID;
            id = nlp_get32(t.value);
            break;
        default:
            if (t.tag & 0x8000)
                return -NLP_UNSUPPORTED;
            break;
        }
    }
    if (next < 0)
        return next;
    int lower = memcmp(e->config.uid, e->peer_uid, 16) < 0;
    if (!a->session_id) {
        if (f->h.opcode != NLP_OP_AUTH_START || stage != 1)
            return -NLP_INVALID;
        error = a->crypto->hash(a->peer_pin, 32, digest);
        if (error || memcmp(digest, e->peer_uid, 16))
            return -NLP_DENIED;
        if (lower) {
            if (size || id || e->next_session == UINT32_MAX)
                return -NLP_CONFLICT;
            a->session_id = e->next_session++;
        } else {
            if (size != 32 || !id || id < e->peer_next_session || id == UINT32_MAX)
                return -NLP_CONFLICT;
            a->session_id = id;
            e->peer_next_session = id + 1;
        }
        error = nlp_noise_init(&a->noise, a->crypto, lower, a->static_key, p,
                               prologue(e, a->session_id, p), verify, a);
        if (error) {
            nlp_auth_reset(a);
            return error;
        }
    } else if (id != a->session_id)
        return -NLP_CONFLICT;
    if (lower && f->h.opcode == NLP_OP_AUTH_START && !size) {
        error = nlp_noise_write(&a->noise, bytes, &written);
        if (!error)
            error = reply(e, f, NLP_OP_AUTH_START, 1, bytes, written);
    } else if (!lower && f->h.opcode == NLP_OP_AUTH_START && stage == 1 && size == 32) {
        error = nlp_noise_read(&a->noise, message, size);
        if (!error)
            error = nlp_noise_write(&a->noise, bytes, &written);
        if (!error)
            error = reply(e, f, NLP_OP_AUTH_STEP, 2, bytes, written);
    } else if (lower && f->h.opcode == NLP_OP_AUTH_STEP && stage == 2 && size == 96) {
        error = nlp_noise_read(&a->noise, message, size);
        if (!error)
            error = nlp_noise_write(&a->noise, bytes, &written);
        if (!error)
            error = reply(e, f, NLP_OP_AUTH_FINISH, 3, bytes, written);
    } else if (!lower && f->h.opcode == NLP_OP_AUTH_FINISH && stage == 3 && size == 64) {
        error = nlp_noise_read(&a->noise, message, size);
        a->reply_size = 0;
    } else
        return -NLP_INVALID;
    if (!error || error == -NLP_BUSY) {
        memcpy(a->request, f->payload, f->length);
        a->request_size = f->length;
        a->request_id = request;
        a->request_opcode = f->h.opcode;
    } else
        nlp_auth_reset(a);
    nlp_secure_clear(bytes, sizeof(bytes));
    return error;
}
static size_t context(struct nlp_endpoint *e, struct nlp_binding *b, unsigned ch, int tx,
                      uint8_t info[90])
{
    static const uint8_t device[16] = NLP_UUID_DEVICE;
    memcpy(info, "NLP1/channel", 12);
    nlp_put32(info + 12, b->session.id);
    nlp_put16(info + 16, (uint16_t)ch);
    memcpy(info + 18, tx ? e->config.uid : e->peer_uid, 16);
    memcpy(info + 34, tx ? e->peer_uid : e->config.uid, 16);
    nlp_put64(info + 50, tx ? e->config.boot_id : e->peer_boot);
    nlp_put64(info + 58, tx ? e->peer_boot : e->config.boot_id);
    nlp_put16(info + 66, tx ? b->session.local_cap : b->session.remote_cap);
    nlp_put16(info + 68, tx ? b->session.remote_cap : b->session.local_cap);
    memcpy(info + 70, b->capability ? b->capability->type : device, 16);
    nlp_put16(info + 86, b->capability ? b->capability->major : 1);
    nlp_put16(info + 88, b->capability ? b->capability->minor : 0);
    return 90;
}
int nlp_auth_channel(struct nlp_endpoint *e, struct nlp_binding *b, unsigned index)
{
    struct nlp_auth *a = e->config.auth;
    uint8_t tx[90], rx[90];
    if (!a || !a->noise.complete || index >= NLP_SESSIONS)
        return -NLP_DENIED;
    for (unsigned ch = 0; ch < NLP_CHANNELS; ++ch) {
        context(e, b, ch, 1, tx);
        context(e, b, ch, 0, rx);
        int error =
            nlp_cipher_derive(&a->cipher[index][ch], &a->noise, tx, sizeof(tx), rx, sizeof(rx));
        if (error)
            return error;
        b->session.channel[ch].cipher = &a->cipher[index][ch];
    }
    return 0;
}
int nlp_auth_join_mac(struct nlp_endpoint *e, const struct nlp_header *h, const uint8_t *payload,
                      size_t size, int tx, uint8_t out[32])
{
    struct nlp_auth *a = e->config.auth;
    uint8_t info[81], key[32], bytes[52 + NLP_PAYLOAD_MAX];
    if (!a || !a->noise.complete || size < 44 || size > NLP_PAYLOAD_MAX || !payload || !out)
        return -NLP_DENIED;
    memcpy(info, "NLP1/join", 9);
    nlp_put32(info + 9, a->session_id);
    memcpy(info + 13, tx ? e->config.uid : e->peer_uid, 16);
    memcpy(info + 29, tx ? e->peer_uid : e->config.uid, 16);
    nlp_put64(info + 45, tx ? e->config.boot_id : e->peer_boot);
    nlp_put64(info + 53, tx ? e->peer_boot : e->config.boot_id);
    memcpy(info + 61, e->config.fabric, 16);
    nlp_put32(info + 77, e->config.epoch);
    int error =
        nlp_hkdf(a->crypto, a->noise.hash, tx ? a->noise.tx : a->noise.rx, 32, info, 81, key, 32);
    if (!error)
        error = nlp_header_encode(h, size, bytes);
    if (!error) {
        bytes[7] = 0;
        memset(bytes + 48, 0, 4);
        memcpy(bytes + 52, payload, size - 36);
        error = a->crypto->hmac(key, bytes, 52 + size - 36, out);
    }
    nlp_secure_clear(key, sizeof(key));
    return error;
}
int nlp_auth_join_verify(struct nlp_endpoint *e, const struct nlp_frame *f)
{
    uint8_t expected[32], difference = 0;
    if (f->length < 44 || nlp_get16(f->payload + f->length - 36) != 22 ||
        nlp_get16(f->payload + f->length - 34) != 32)
        return -NLP_DENIED;
    int error = nlp_auth_join_mac(e, &f->h, f->payload, f->length, 0, expected);
    if (error)
        return error;
    for (unsigned i = 0; i < 32; ++i)
        difference |= expected[i] ^ f->payload[f->length - 32 + i];
    return difference ? -NLP_DENIED : 0;
}
