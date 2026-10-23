/* Copyright (C) 2026 NOS contributors. */
#include "nlp/identity.h"
#include <string.h>
static int erased(const uint8_t *bytes, size_t size)
{
    while (size--)
        if (*bytes++ != 0xff)
            return 0;
    return 1;
}
int nlp_identity_boot(const struct nlp_nv *nv, uint32_t base, const uint8_t uid[16], uint64_t *boot)
{
    uint8_t record[32], verify[32];
    uint64_t latest = 0;
    uint32_t free_at[2] = {UINT32_MAX, UINT32_MAX}, latest_sector = 0;
    static const uint8_t magic[4] = {'N', 'L', 'P', 'B'};
    if (!nv || !nv->read || !nv->write || !nv->erase || !uid || !boot || (base & 4095) ||
        base > UINT32_MAX - 8192)
        return -NLP_INVALID;
    for (unsigned sector = 0; sector < 2; ++sector)
        for (unsigned slot = 0; slot < 128; ++slot) {
            uint32_t at = base + sector * 4096 + slot * 32;
            if (nv->read(nv->context, at, record, 32))
                return -NLP_INTEGRITY;
            if (erased(record, 32)) {
                if (free_at[sector] == UINT32_MAX)
                    free_at[sector] = at;
                continue;
            }
            if (memcmp(record, magic, 4)) {
                /* Only an ordered prefix of our first program transaction may be torn. */
                unsigned prefix = 0;
                while (prefix < 4 && record[prefix] == magic[prefix])
                    ++prefix;
                if (!prefix || !erased(record + prefix, 32 - prefix))
                    return -NLP_CONFLICT;
                continue;
            }
            if (nlp_get32(record + 28) != nlp_crc32c(record, 28))
                continue;
            if (memcmp(record + 4, uid, 16) || !nlp_get64(record + 20))
                return -NLP_CONFLICT;
            if (nlp_get64(record + 20) > latest) {
                latest = nlp_get64(record + 20);
                latest_sector = sector;
            }
        }
    if (latest == UINT64_MAX)
        return -NLP_RANGE;
    uint32_t target = free_at[latest_sector];
    if (target == UINT32_MAX) {
        unsigned other = latest_sector ^ 1;
        /* Latest valid record is retained in the other sector during erase. */
        target = base + other * 4096;
        if (nv->erase(nv->context, target, 4096))
            return -NLP_INTEGRITY;
    }
    memcpy(record, magic, 4);
    memcpy(record + 4, uid, 16);
    nlp_put64(record + 20, latest + 1);
    nlp_put32(record + 28, nlp_crc32c(record, 28));
    if (nv->write(nv->context, target, record, 28) ||
        nv->write(nv->context, target + 28, record + 28, 4) ||
        nv->read(nv->context, target, verify, 32) || memcmp(verify, record, 32))
        return -NLP_INTEGRITY;
    *boot = latest + 1;
    return 0;
}
