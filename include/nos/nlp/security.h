/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_SECURITY_H
#define NOS_NLP_SECURITY_H
#include "wire.h"
/* Implement with a maintained crypto library. No default weak/random fallback. */
struct nlp_crypto {
    int (*hash)(const uint8_t *, size_t, uint8_t out[32]);
    int (*hmac)(const uint8_t key[32], const uint8_t *, size_t, uint8_t out[32]);
    int (*public_key)(const uint8_t private_key[32], uint8_t public_key[32]);
    int (*dh)(const uint8_t private_key[32], const uint8_t public_key[32], uint8_t shared[32]);
    int (*aead)(int decrypt, const uint8_t key[32], const uint8_t nonce[12], const uint8_t *,
                size_t, const uint8_t *, size_t, uint8_t *);
    int (*random)(uint8_t *, size_t);
};
typedef int (*nlp_verify_key_fn)(void *, const uint8_t peer_static[32]);
struct nlp_noise {
    const struct nlp_crypto *crypto;
    nlp_verify_key_fn verify;
    void *context;
    uint8_t ck[32], hash[32], key[32], e[32], ep[32], re[32], s[32], sp[32], rs[32];
    uint8_t tx[32], rx[32];
    uint64_t nonce;
    uint8_t initiator, step, keyed, complete, failed;
};
struct nlp_cipher {
    const struct nlp_crypto *crypto;
    uint8_t tx_key[32], rx_key[32], tx_prefix[6], rx_prefix[6];
};
int nlp_hkdf(const struct nlp_crypto *, const uint8_t salt[32], const uint8_t *, size_t,
             const uint8_t *, size_t, uint8_t *, size_t);
void nlp_secure_clear(void *, size_t);
int nlp_noise_init(struct nlp_noise *, const struct nlp_crypto *, int initiator,
                   const uint8_t static_key[32], const uint8_t *, size_t, nlp_verify_key_fn,
                   void *);
int nlp_noise_write(struct nlp_noise *, uint8_t out[96], size_t *);
int nlp_noise_read(struct nlp_noise *, const uint8_t *, size_t);
/* info is the exact channel KDF context from security.md, including label. */
int nlp_cipher_derive(struct nlp_cipher *, const struct nlp_noise *, const uint8_t *tx_info, size_t,
                      const uint8_t *rx_info, size_t);
int nlp_cipher_encode(const struct nlp_cipher *, const struct nlp_header *, const void *, size_t,
                      uint8_t *, size_t, size_t *);
int nlp_cipher_decode(const struct nlp_cipher *, const uint8_t *, size_t, struct nlp_frame *,
                      uint8_t plaintext[NLP_PAYLOAD_MAX]);
#endif
