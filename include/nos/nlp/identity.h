/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_IDENTITY_H
#define NOS_NLP_IDENTITY_H
#include "wire.h"
struct nlp_nv {
    void *context;
    int (*read)(void *, uint32_t, void *, size_t);
    int (*write)(void *, uint32_t, const void *, size_t);
    int (*erase)(void *, uint32_t, size_t);
};
/* Two explicit 4KiB reserved sectors. Unknown contents are NEVER erased.
 * Returns a new boot only after a committed, read-back-verified record. */
int nlp_identity_boot(const struct nlp_nv *, uint32_t base, const uint8_t uid[16],
                      uint64_t *boot_id);
#endif
