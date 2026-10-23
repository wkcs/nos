/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_HASH_H
#define NOS_NLP_HASH_H
#include <stddef.h>
#include <stdint.h>
/* TinyCrypt SHA-256 for bounded MCU blobs. Does not enable an authenticated profile. */
int nlp_sha256(const void *, size_t, uint8_t digest[32]);
#endif
