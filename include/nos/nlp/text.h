/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_TEXT_H
#define NOS_NLP_TEXT_H
#include <stddef.h>
#include <stdint.h>
/* Validate UTF-8 and truncate only at a character boundary; bounded to 4096 input bytes. */
int nlp_utf8_prefix(const uint8_t *, size_t input_size, size_t limit, size_t *written);
#endif
