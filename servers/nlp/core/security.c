/* Copyright (C) 2026 NOS contributors. Noise XX state machine; crypto is supplied by a library
 * backend. */
#include "nlp/security.h"
#include <string.h>
void nlp_secure_clear(void *memory, size_t size)
{
    volatile uint8_t *p = memory;
    while (size--)
        *p++ = 0;
}
static int mix_hash(struct nlp_noise *n, const uint8_t *data, size_t size)
{
    uint8_t bytes[32 + 256];
    int error;
    if (size > 256)
        return -NLP_RANGE;
    memcpy(bytes, n->hash, 32);
    if (size)
        memcpy(bytes + 32, data, size);
    error = n->crypto->hash(bytes, 32 + size, n->hash);
    nlp_secure_clear(bytes, sizeof(bytes));
    return error;
}
int nlp_hkdf(const struct nlp_crypto *c, const uint8_t salt[32], const uint8_t *input, size_t size,
             const uint8_t *info, size_t info_size, uint8_t *out, size_t wanted)
{
    uint8_t prk[32], block[32], bytes[32 + 160 + 1];
    size_t done = 0, previous = 0;
    int error;
    if (info_size > 160 || wanted > 64)
        return -NLP_RANGE;
    error = c->hmac(salt, input, size, prk);
    for (uint8_t i = 1; !error && done < wanted; ++i) {
        if (previous)
            memcpy(bytes, block, 32);
        if (info_size)
            memcpy(bytes + previous, info, info_size);
        bytes[previous + info_size] = i;
        error = c->hmac(prk, bytes, previous + info_size + 1, block);
        if (!error) {
            size_t copy = wanted - done > 32 ? 32 : wanted - done;
            memcpy(out + done, block, copy);
            done += copy;
            previous = 32;
        }
    }
    nlp_secure_clear(prk, 32);
    nlp_secure_clear(block, 32);
    nlp_secure_clear(bytes, sizeof(bytes));
    return error;
}
static int mix_dh(struct nlp_noise *n, const uint8_t private_key[32], const uint8_t public_key[32])
{
    uint8_t shared[32], keys[64];
    int error = n->crypto->dh(private_key, public_key, shared);
    if (!error)
        error = nlp_hkdf(n->crypto, n->ck, shared, 32, NULL, 0, keys, 64);
    if (!error) {
        memcpy(n->ck, keys, 32);
        memcpy(n->key, keys + 32, 32);
        n->nonce = 0;
        n->keyed = 1;
    }
    nlp_secure_clear(shared, 32);
    nlp_secure_clear(keys, 64);
    return error;
}
static int crypt_hash(struct nlp_noise *n, int decrypt, const uint8_t *input, size_t size,
                      uint8_t *out)
{
    uint8_t nonce[12] = {0};
    size_t cipher_size = decrypt ? size : size + 16;
    int error;
    if (!n->keyed || (decrypt && size < 16))
        return -NLP_INVALID;
    nlp_put64(nonce + 4, n->nonce++);
    error = n->crypto->aead(decrypt, n->key, nonce, n->hash, 32, input, size, out);
    if (!error)
        error = mix_hash(n, decrypt ? input : out, cipher_size);
    return error;
}
static int finish(struct nlp_noise *n)
{
    uint8_t keys[64];
    int error = nlp_hkdf(n->crypto, n->ck, NULL, 0, NULL, 0, keys, 64);
    if (!error) {
        memcpy(n->tx, keys + (n->initiator ? 0 : 32), 32);
        memcpy(n->rx, keys + (n->initiator ? 32 : 0), 32);
        n->complete = 1;
    }
    nlp_secure_clear(keys, 64);
    nlp_secure_clear(n->e, 32);
    nlp_secure_clear(n->s, 32);
    nlp_secure_clear(n->key, 32);
    nlp_secure_clear(n->ck, 32);
    return error;
}
static int fail(struct nlp_noise *n, int error)
{
    if (error) {
        nlp_secure_clear(n, sizeof(*n));
        n->failed = 1;
    }
    return error;
}
int nlp_noise_init(struct nlp_noise *n, const struct nlp_crypto *c, int initiator,
                   const uint8_t static_key[32], const uint8_t *prologue, size_t size,
                   nlp_verify_key_fn verify, void *context)
{
    static const uint8_t name[32] = "Noise_XX_25519_ChaChaPoly_SHA256";
    int error;
    if (!n || !c || !c->hash || !c->hmac || !c->public_key || !c->dh || !c->aead || !c->random ||
        !static_key || (!prologue && size) || !verify)
        return -NLP_INVALID;
    memset(n, 0, sizeof(*n));
    n->crypto = c;
    n->verify = verify;
    n->context = context;
    n->initiator = !!initiator;
    memcpy(n->ck, name, 32);
    memcpy(n->hash, name, 32);
    memcpy(n->s, static_key, 32);
    error = c->public_key(n->s, n->sp);
    if (!error)
        error = c->random(n->e, 32);
    if (!error)
        error = c->public_key(n->e, n->ep);
    if (!error)
        error = mix_hash(n, prologue, size);
    return fail(n, error);
}
int nlp_noise_write(struct nlp_noise *n, uint8_t out[96], size_t *size)
{
    int error = 0;
    if (!n || !out || !size || n->failed || n->complete)
        return -NLP_INVALID;
    if (n->initiator && n->step == 0) {
        memcpy(out, n->ep, 32);
        error = mix_hash(n, out, 32);
        *size = 32;
    } else if (!n->initiator && n->step == 1) {
        memcpy(out, n->ep, 32);
        error = mix_hash(n, out, 32);
        if (!error)
            error = mix_dh(n, n->e, n->re);
        if (!error)
            error = crypt_hash(n, 0, n->sp, 32, out + 32);
        if (!error)
            error = mix_dh(n, n->s, n->re);
        if (!error)
            error = crypt_hash(n, 0, NULL, 0, out + 80);
        *size = 96;
    } else if (n->initiator && n->step == 2) {
        error = crypt_hash(n, 0, n->sp, 32, out);
        if (!error)
            error = mix_dh(n, n->s, n->re);
        if (!error)
            error = crypt_hash(n, 0, NULL, 0, out + 48);
        if (!error)
            error = finish(n);
        *size = 64;
    } else
        return -NLP_INVALID;
    if (!error)
        ++n->step;
    return fail(n, error);
}
int nlp_noise_read(struct nlp_noise *n, const uint8_t *in, size_t size)
{
    uint8_t empty[16];
    int error;
    if (!n || !in || n->failed || n->complete)
        return -NLP_INVALID;
    if (!n->initiator && n->step == 0 && size == 32) {
        memcpy(n->re, in, 32);
        error = mix_hash(n, in, 32);
    } else if (n->initiator && n->step == 1 && size == 96) {
        memcpy(n->re, in, 32);
        error = mix_hash(n, in, 32);
        if (!error)
            error = mix_dh(n, n->e, n->re);
        if (!error)
            error = crypt_hash(n, 1, in + 32, 48, n->rs);
        if (!error)
            error = n->verify(n->context, n->rs);
        if (!error)
            error = mix_dh(n, n->e, n->rs);
        if (!error)
            error = crypt_hash(n, 1, in + 80, 16, empty);
    } else if (!n->initiator && n->step == 2 && size == 64) {
        error = crypt_hash(n, 1, in, 48, n->rs);
        if (!error)
            error = n->verify(n->context, n->rs);
        if (!error)
            error = mix_dh(n, n->e, n->rs);
        if (!error)
            error = crypt_hash(n, 1, in + 48, 16, empty);
        if (!error)
            error = finish(n);
    } else
        return fail(n, -NLP_INVALID);
    if (!error)
        ++n->step;
    nlp_secure_clear(empty, sizeof(empty));
    return fail(n, error);
}
int nlp_cipher_derive(struct nlp_cipher *c, const struct nlp_noise *n, const uint8_t *tx,
                      size_t tx_size, const uint8_t *rx, size_t rx_size)
{
    uint8_t bytes[38];
    int error;
    if (!c || !n || !n->complete || !tx || !rx)
        return -NLP_DENIED;
    memset(c, 0, sizeof(*c));
    c->crypto = n->crypto;
    error = nlp_hkdf(c->crypto, n->hash, n->tx, 32, tx, tx_size, bytes, 38);
    if (!error) {
        memcpy(c->tx_key, bytes, 32);
        memcpy(c->tx_prefix, bytes + 32, 6);
    }
    if (!error)
        error = nlp_hkdf(c->crypto, n->hash, n->rx, 32, rx, rx_size, bytes, 38);
    if (!error) {
        memcpy(c->rx_key, bytes, 32);
        memcpy(c->rx_prefix, bytes + 32, 6);
    }
    nlp_secure_clear(bytes, sizeof(bytes));
    if (error)
        nlp_secure_clear(c, sizeof(*c));
    return error;
}
int nlp_cipher_encode(const struct nlp_cipher *c, const struct nlp_header *h, const void *data,
                      size_t length, uint8_t *out, size_t capacity, size_t *written)
{
    uint8_t aad[52], nonce[12];
    int error;
    if (!c || !c->crypto || !h || !(h->flags & NLP_ENCRYPTED) || !out || !written ||
        (!data && length))
        return -NLP_DENIED;
    if (length > NLP_PAYLOAD_MAX || capacity < 68u + length)
        return -NLP_NO_SPACE;
    error = nlp_header_encode(h, length, out);
    if (error)
        return error;
    nlp_header_aad(out, aad);
    memcpy(nonce, c->tx_prefix, 6);
    nlp_put16(nonce + 6, h->channel_id);
    nlp_put32(nonce + 8, h->packet_counter);
    error = c->crypto->aead(0, c->tx_key, nonce, aad, 52, data, length, out + 52);
    if (!error)
        *written = length + 68;
    return error;
}
int nlp_cipher_decode(const struct nlp_cipher *c, const uint8_t *raw, size_t size,
                      struct nlp_frame *f, uint8_t plaintext[NLP_PAYLOAD_MAX])
{
    uint8_t aad[52], nonce[12];
    int error;
    if (!c || !c->crypto || !plaintext)
        return -NLP_DENIED;
    error = nlp_frame_inspect(raw, size, f);
    if (error)
        return error;
    if (!(f->h.flags & NLP_ENCRYPTED))
        return -NLP_DENIED;
    nlp_header_aad(raw, aad);
    memcpy(nonce, c->rx_prefix, 6);
    nlp_put16(nonce + 6, f->h.channel_id);
    nlp_put32(nonce + 8, f->h.packet_counter);
    error = c->crypto->aead(1, c->rx_key, nonce, aad, 52, raw + 52, f->length + 16, plaintext);
    if (!error)
        f->payload = plaintext;
    else
        nlp_secure_clear(plaintext, NLP_PAYLOAD_MAX);
    return error;
}
