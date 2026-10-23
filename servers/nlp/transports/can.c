/* Copyright (C) 2026 NOS contributors. */
#include "nlp/transport.h"
#include <string.h>
int nlp_can_id(unsigned priority, unsigned source, unsigned destination, unsigned transfer,
               unsigned subtype, uint32_t *id)
{
    if (!id || priority > 3 || source > 253 || destination > 255 || destination == 254 ||
        transfer > 63 || subtype > 1)
        return -NLP_INVALID;
    *id = priority << 26 | source << 18 | destination << 10 | transfer << 4 | subtype;
    return 0;
}
uint32_t nlp_can_commission_reply_id(uint8_t byte, uint8_t token)
{
    return 255u << 18 | 255u << 10 | (uint32_t)byte << 2 | (token & 3u);
}
void nlp_can_reset(struct nlp_can *c, uint8_t address)
{
    uint32_t generation = c->generation + 1;
    memset(c, 0, sizeof(*c));
    c->generation = generation;
    c->address = address;
}
int nlp_can_fragment(const uint8_t *in, size_t length, unsigned mtu, unsigned index,
                     uint8_t out[64], size_t *written)
{
    size_t offset, count;
    unsigned header = index ? 2 : 4;
    if (!in || !out || !written || (mtu != 8 && mtu != 64) || index > 255 || !length ||
        length > NLP_PDU_MAX)
        return -NLP_INVALID;
    offset = index ? mtu - 4u + (index - 1u) * (mtu - 2u) : 0;
    if (offset >= length)
        return -NLP_RANGE;
    count = length - offset;
    if (count > mtu - header)
        count = mtu - header;
    out[0] = (uint8_t)index;
    out[1] = (index ? 0 : 1) | (offset + count == length ? 2 : 0);
    if (!index)
        nlp_put16(out + 2, (uint16_t)length);
    memcpy(out + header, in + offset, count);
    *written = count + header;
    if (mtu == 64 && *written > 8) {
        static const uint8_t sizes[] = {12, 16, 20, 24, 32, 48, 64};
        for (unsigned i = 0; i < sizeof(sizes); ++i)
            if (*written <= sizes[i]) {
                memset(out + *written, 0, sizes[i] - *written);
                *written = sizes[i];
                break;
            }
    }
    return 0;
}
int nlp_can_receive(struct nlp_can *c, uint32_t id, const uint8_t *data, size_t size, uint32_t now,
                    nlp_pdu_fn receive, void *context)
{
    struct nlp_can_slot *slot = NULL, *free_slot = NULL;
    unsigned source = (id >> 18) & 255, destination = (id >> 10) & 255;
    unsigned transfer = (id >> 4) & 63, subtype = id & 15;
    size_t header, count;
    if (!c || !data || !receive || id >= (1u << 29) || id >> 26 > 3 || source > 253 ||
        subtype > 1 || (destination != c->address && destination != 255) || size < 2 || size > 64 ||
        (data[1] & ~7u))
        return -NLP_INVALID;
    if (size > 8 && size != 12 && size != 16 && size != 20 && size != 24 && size != 32 &&
        size != 48 && size != 64)
        return -NLP_INVALID;
    for (unsigned i = 0; i < NLP_CAN_SLOTS; ++i) {
        struct nlp_can_slot *s = &c->slot[i];
        if (s->active && now - s->started_ms >= 100)
            s->active = 0;
        if (!s->active)
            free_slot = s;
        else if (s->source == source && s->destination == destination && s->transfer == transfer &&
                 s->subtype == subtype)
            slot = s;
    }
    if (data[1] & 4u) {
        if (data[1] != 4 || size != 2)
            return -NLP_INVALID;
        if (slot)
            slot->active = 0;
        return 0;
    }
    if (slot && data[0] + 1u == slot->next) {
        if (slot->last_size == size && !memcmp(slot->last, data, size))
            return 0;
        slot->active = 0;
        return -NLP_CONFLICT;
    }
    header = data[1] & 1u ? 4 : 2;
    if (header == 4) {
        if (size < 4 || data[0] || nlp_get16(data + 2) > (subtype ? 64 : NLP_PDU_MAX) ||
            !nlp_get16(data + 2))
            return -NLP_INVALID;
        if (!slot)
            slot = free_slot;
        if (!slot)
            return -NLP_BUSY;
        memset(slot, 0, sizeof(*slot));
        slot->active = 1;
        slot->source = (uint8_t)source;
        slot->destination = (uint8_t)destination;
        slot->transfer = (uint8_t)transfer;
        slot->subtype = (uint8_t)subtype;
        slot->expected = nlp_get16(data + 2);
        slot->started_ms = now;
    }
    if (!slot)
        return -NLP_NOT_FOUND;
    if (data[0] != slot->next) {
        slot->active = 0;
        return -NLP_INVALID;
    }
    count = size - header;
    if (count > (size_t)(slot->expected - slot->used)) {
        size_t padding = count - (slot->expected - slot->used);
        if (!(data[1] & 2)) {
            slot->active = 0;
            return -NLP_INVALID;
        }
        for (size_t i = size - padding; i < size; ++i)
            if (data[i]) {
                slot->active = 0;
                return -NLP_INVALID;
            }
        count -= padding;
    }
    memcpy(slot->bytes + slot->used, data + header, count);
    slot->used += (uint16_t)count;
    memcpy(slot->last, data, size);
    slot->last_size = (uint8_t)size;
    ++slot->next;
    if ((slot->used == slot->expected) != !!(data[1] & 2)) {
        slot->active = 0;
        return -NLP_INVALID;
    }
    if (data[1] & 2) {
        /* Caller must accept/drop the complete PDU now; end-to-end retry handles BUSY. */
        slot->active = 0;
        return receive(context, slot->bytes, slot->used);
    }
    if (slot->next > 255) {
        slot->active = 0;
        return -NLP_RANGE;
    }
    return 0;
}
