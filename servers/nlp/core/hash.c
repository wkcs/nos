/* Copyright (C) 2026 NOS contributors. */
#include "nlp/hash.h"
#include "nlp/registry.h"
#include <tinycrypt/sha256.h>
#include <tinycrypt/constants.h>
int nlp_sha256(const void *bytes, size_t size, uint8_t digest[32])
{
    struct tc_sha256_state_struct state;
    if ((!bytes && size) || !digest || tc_sha256_init(&state) != TC_CRYPTO_SUCCESS ||
        (size && tc_sha256_update(&state, bytes, size) != TC_CRYPTO_SUCCESS) ||
        tc_sha256_final(digest, &state) != TC_CRYPTO_SUCCESS)
        return -NLP_INTEGRITY;
    return 0;
}
