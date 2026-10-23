/* Copyright (C) 2026 NOS contributors. Crypto primitives only; no platform fallback. */
#include "nlp/security.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <limits.h>
static int hash(const uint8_t *in, size_t size, uint8_t out[32])
{
    unsigned length = 0;
    return EVP_Digest(in, size, out, &length, EVP_sha256(), NULL) && length == 32 ? 0
                                                                                  : -NLP_INTEGRITY;
}
static int hmac(const uint8_t key[32], const uint8_t *in, size_t size, uint8_t out[32])
{
    unsigned length = 0;
    return HMAC(EVP_sha256(), key, 32, in, size, out, &length) && length == 32 ? 0 : -NLP_INTEGRITY;
}
static int public_key(const uint8_t private_key[32], uint8_t public_key[32])
{
    EVP_PKEY *p = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, 32);
    size_t size = 32;
    int good = p && EVP_PKEY_get_raw_public_key(p, public_key, &size) && size == 32;
    EVP_PKEY_free(p);
    return good ? 0 : -NLP_INTEGRITY;
}
static int dh(const uint8_t private_key[32], const uint8_t public_key[32], uint8_t shared[32])
{
    EVP_PKEY *local = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, 32);
    EVP_PKEY *remote = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, public_key, 32);
    EVP_PKEY_CTX *ctx = local ? EVP_PKEY_CTX_new(local, NULL) : NULL;
    size_t size = 32;
    int good = ctx && remote && EVP_PKEY_derive_init(ctx) > 0 &&
               EVP_PKEY_derive_set_peer(ctx, remote) > 0 &&
               EVP_PKEY_derive(ctx, shared, &size) > 0 && size == 32;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(remote);
    EVP_PKEY_free(local);
    return good ? 0 : -NLP_INTEGRITY;
}
static int aead(int decrypt, const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
                size_t aad_size, const uint8_t *in, size_t size, uint8_t *out)
{
    EVP_CIPHER_CTX *ctx;
    int good, written = 0, end = 0, count = 0;
    size_t length = decrypt && size >= 16 ? size - 16 : size;
    if (size > INT_MAX || aad_size > INT_MAX || (decrypt && size < 16))
        return -NLP_INVALID;
    ctx = EVP_CIPHER_CTX_new();
    good = ctx && EVP_CipherInit_ex(ctx, EVP_chacha20_poly1305(), NULL, key, nonce, !decrypt);
    if (good && aad_size)
        good = EVP_CipherUpdate(ctx, NULL, &count, aad, (int)aad_size);
    if (good && length)
        good = EVP_CipherUpdate(ctx, out, &written, in, (int)length);
    if (good && decrypt)
        good = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, (void *)(in + length));
    if (good)
        good = EVP_CipherFinal_ex(ctx, out + written, &end);
    if (good && !decrypt)
        good = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out + length);
    EVP_CIPHER_CTX_free(ctx);
    return good ? 0 : -NLP_INTEGRITY;
}
static int random_bytes(uint8_t *out, size_t size)
{
    return size <= INT_MAX && RAND_bytes(out, (int)size) == 1 ? 0 : -NLP_INTEGRITY;
}
const struct nlp_crypto nlp_openssl_crypto = {hash, hmac, public_key, dh, aead, random_bytes};
