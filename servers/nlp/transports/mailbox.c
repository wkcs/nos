/* Copyright (C) 2026 NOS contributors. */
#include "nlp/transport.h"
#include <string.h>
void nlp_mailbox_init(struct nlp_mailbox *m, uint64_t boot_id)
{
    memset(m, 0, sizeof(*m));
    m->boot_id = boot_id;
    m->sequence = 1;
}
int nlp_mailbox_enqueue(struct nlp_mailbox *m, const uint8_t *data, size_t length)
{
    struct nlp_frame frame;
    if (!m || !m->boot_id || nlp_frame_inspect(data, length, &frame))
        return -NLP_INVALID;
    if (m->count == 2 || m->sequence >= UINT32_MAX - 2)
        return -NLP_BUSY;
    unsigned tail = (m->head + m->count) % 2;
    memcpy(m->queue[tail], data, length);
    m->lengths[tail] = (uint16_t)length;
    ++m->count;
    return 0;
}
static void token(struct nlp_mailbox *m, uint8_t *out)
{
    memset(out, 0, 12);
    if (m->count) {
        nlp_put64(out, m->boot_id);
        nlp_put32(out + 8, m->sequence);
    }
}
int nlp_mailbox_read(struct nlp_mailbox *m, unsigned reg, uint8_t *out, size_t size)
{
    static const uint8_t id[8] = {'N', 'L', 'P', 'I', 1, 0, 0, 0};
    if (!m || !out)
        return -NLP_INVALID;
    if (!reg && size == 8) {
        memcpy(out, id, 8);
        return 0;
    }
    if (reg == 0x10 && size == 20) {
        memset(out, 0, 20);
        nlp_put16(out, 1u | (m->count ? 2u : 0u));
        nlp_put16(out + 2, NLP_PDU_MAX);
        nlp_put16(out + 4, NLP_PDU_MAX);
        nlp_put16(out + 6, m->count ? m->lengths[m->head] : 0);
        token(m, out + 8);
        return 0;
    }
    if (reg == 0x30 && size == 14u + (m->count ? m->lengths[m->head] : 0u)) {
        token(m, out);
        nlp_put16(out + 12, m->count ? m->lengths[m->head] : 0);
        if (m->count)
            memcpy(out + 14, m->queue[m->head], m->lengths[m->head]);
        return 0;
    }
    return -NLP_INVALID;
}
int nlp_mailbox_write(struct nlp_mailbox *m, unsigned reg, const uint8_t *in, size_t size,
                      nlp_pdu_fn receive, void *context)
{
    uint8_t current[12];
    if (!m || (!in && size))
        return -NLP_INVALID;
    if (reg == 0x20 && size >= 2 && size - 2 == nlp_get16(in) && receive) {
        struct nlp_frame frame;
        int error = nlp_frame_inspect(in + 2, size - 2, &frame);
        if (error)
            return error;
        return receive(context, in + 2, size - 2);
    }
    if ((reg == 0x40 || reg == 0x50) && size == 12) {
        token(m, current);
        if (m->count && !memcmp(current, in, 12)) {
            m->head ^= 1;
            --m->count;
            ++m->sequence;
        }
        return 0; /* Late or repeated token never pops the next record. */
    }
    return -NLP_INVALID;
}
