/* Copyright (C) 2026 NOS contributors. */
#include "nlp/wire.h"
#include <string.h>
uint16_t nlp_get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
uint32_t nlp_get32(const uint8_t *p)
{
    return (uint32_t)nlp_get16(p) | (uint32_t)nlp_get16(p + 2) << 16;
}
uint64_t nlp_get64(const uint8_t *p)
{
    return (uint64_t)nlp_get32(p) | (uint64_t)nlp_get32(p + 4) << 32;
}
void nlp_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
void nlp_put32(uint8_t *p, uint32_t v)
{
    nlp_put16(p, (uint16_t)v);
    nlp_put16(p + 2, (uint16_t)(v >> 16));
}
void nlp_put64(uint8_t *p, uint64_t v)
{
    nlp_put32(p, (uint32_t)v);
    nlp_put32(p + 4, (uint32_t)(v >> 32));
}
uint32_t nlp_crc32c(const void *data, size_t length)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (length--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
    return crc ^ UINT32_MAX;
}
static int valid(const struct nlp_header *h, size_t length)
{
    if (!h || length > NLP_PAYLOAD_MAX || h->kind > NLP_CLOSE || (h->flags & ~3u) ||
        h->priority > 3 || !h->hop_limit || h->hop_limit > 16 || !h->src_node || !h->dst_node ||
        h->src_cap == UINT16_MAX || h->dst_cap == UINT16_MAX)
        return -NLP_INVALID;
    if (!h->session_id) {
        if (h->kind != NLP_CONTROL || h->flags || h->src_cap || h->dst_cap || h->channel_id ||
            h->packet_counter || h->reliable_seq || h->ack || h->receive_limit)
            return -NLP_INVALID;
    } else if (!h->fabric_epoch || !h->packet_counter || h->src_node >= 65534 ||
               h->dst_node >= 65534) {
        return -NLP_INVALID;
    }
    if ((h->flags & NLP_RELIABLE) ? (!h->reliable_seq || h->kind == NLP_ACK) : h->reliable_seq != 0)
        return -NLP_INVALID;
    if (h->receive_limit < h->ack || h->receive_limit - h->ack > 8)
        return -NLP_INVALID;
    if (h->kind == NLP_ACK) {
        if (length || h->opcode || !h->session_id)
            return -NLP_INVALID;
    } else if (!(h->flags & NLP_RELIABLE) && (h->ack || h->receive_limit)) {
        return -NLP_INVALID;
    }
    return 0;
}
int nlp_header_encode(const struct nlp_header *h, size_t length, uint8_t out[52])
{
    if (!out || valid(h, length))
        return -NLP_INVALID;
    memset(out, 0, 52);
    out[0] = 'N';
    out[1] = 'L';
    out[2] = 1;
    out[3] = 52;
    out[4] = h->kind;
    out[5] = h->flags;
    out[6] = h->priority;
    out[7] = h->hop_limit;
    nlp_put32(out + 8, h->fabric_epoch);
    nlp_put16(out + 12, h->src_node);
    nlp_put16(out + 14, h->dst_node);
    nlp_put16(out + 16, h->src_cap);
    nlp_put16(out + 18, h->dst_cap);
    nlp_put16(out + 20, h->channel_id);
    nlp_put16(out + 22, h->opcode);
    nlp_put32(out + 24, h->session_id);
    nlp_put32(out + 28, h->packet_counter);
    nlp_put32(out + 32, h->reliable_seq);
    nlp_put32(out + 36, h->ack);
    nlp_put16(out + 40, (uint16_t)length);
    nlp_put32(out + 44, h->receive_limit);
    nlp_put32(out + 48, nlp_crc32c(out, 48));
    return 0;
}
int nlp_frame_inspect(const uint8_t *p, size_t size, struct nlp_frame *f)
{
    struct nlp_header h;
    uint16_t length;
    if (!p || !f || size < 56 || size > NLP_PDU_MAX || p[0] != 'N' || p[1] != 'L' || p[2] != 1 ||
        p[3] != 52 || nlp_get16(p + 42))
        return -NLP_INVALID;
    if (nlp_get32(p + 48) != nlp_crc32c(p, 48))
        return -NLP_INTEGRITY;
    memset(&h, 0, sizeof(h));
    h.kind = p[4];
    h.flags = p[5];
    h.priority = p[6];
    h.hop_limit = p[7];
    h.fabric_epoch = nlp_get32(p + 8);
    h.src_node = nlp_get16(p + 12);
    h.dst_node = nlp_get16(p + 14);
    h.src_cap = nlp_get16(p + 16);
    h.dst_cap = nlp_get16(p + 18);
    h.channel_id = nlp_get16(p + 20);
    h.opcode = nlp_get16(p + 22);
    h.session_id = nlp_get32(p + 24);
    h.packet_counter = nlp_get32(p + 28);
    h.reliable_seq = nlp_get32(p + 32);
    h.ack = nlp_get32(p + 36);
    h.receive_limit = nlp_get32(p + 44);
    length = nlp_get16(p + 40);
    if (valid(&h, length) || size != 52u + length + ((h.flags & NLP_ENCRYPTED) ? 16u : 4u))
        return -NLP_INVALID;
    f->h = h;
    f->length = length;
    f->payload = p + 52;
    return 0;
}
int nlp_frame_encode(const struct nlp_header *h, const void *data, size_t length, uint8_t *out,
                     size_t capacity, size_t *written)
{
    if (!h || !out || !written || (!data && length))
        return -NLP_INVALID;
    if (h->flags & NLP_ENCRYPTED)
        return -NLP_UNSUPPORTED;
    if (length > NLP_PAYLOAD_MAX || capacity < length + 56)
        return -NLP_NO_SPACE;
    if (nlp_header_encode(h, length, out))
        return -NLP_INVALID;
    if (length)
        memcpy(out + 52, data, length);
    nlp_put32(out + 52 + length, nlp_crc32c(out, 52 + length));
    *written = length + 56;
    return 0;
}
int nlp_frame_decode(const uint8_t *bytes, size_t length, struct nlp_frame *f)
{
    int result = nlp_frame_inspect(bytes, length, f);
    if (result)
        return result;
    if (f->h.flags & NLP_ENCRYPTED)
        return -NLP_UNSUPPORTED;
    return nlp_get32(bytes + length - 4) == nlp_crc32c(bytes, length - 4) ? 0 : -NLP_INTEGRITY;
}
void nlp_header_aad(const uint8_t in[52], uint8_t out[52])
{
    memcpy(out, in, 52);
    out[7] = 0;
    memset(out + 48, 0, 4);
}
int nlp_tlv_next(struct nlp_tlv_reader *r, struct nlp_tlv *t)
{
    if (!r || !t || (!r->bytes && r->length) || r->length > 1024 || r->offset > r->length)
        return -NLP_INVALID;
    if (r->offset == r->length)
        return 0;
    if (r->length - r->offset < 4 || r->count >= 32)
        return -NLP_INVALID;
    t->tag = nlp_get16(r->bytes + r->offset);
    t->length = nlp_get16(r->bytes + r->offset + 2);
    if ((t->tag & 0x7fffu) <= r->previous || t->length > r->length - r->offset - 4)
        return -NLP_INVALID;
    t->value = r->bytes + r->offset + 4;
    r->offset += 4u + t->length;
    r->previous = t->tag & 0x7fffu;
    ++r->count;
    return 1;
}
int nlp_tlv_add(uint8_t *out, size_t capacity, size_t *used, uint16_t tag, const void *data,
                size_t length)
{
    if (!out || !used || !tag || !(tag & 0x7fffu) || (!data && length) || length > 1024 ||
        *used > capacity)
        return -NLP_INVALID;
    if (capacity - *used < 4 || length > capacity - *used - 4)
        return -NLP_NO_SPACE;
    nlp_put16(out + *used, tag);
    nlp_put16(out + *used + 2, (uint16_t)length);
    if (length)
        memcpy(out + *used + 4, data, length);
    *used += 4 + length;
    return 0;
}
