/* Copyright (C) 2026 NOS contributors */
#include <string.h>
#include "config.h"

#define RECORD_MAGIC 0x30364b4eu
#define RECORD_COMMIT 0x54494d43u
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16;
}
static void put16(uint8_t *p, unsigned v)
{
    p[0] = v;
    p[1] = v >> 8;
}
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}
static int usage(unsigned u)
{
    return u >= 4 && u <= 0xe7;
}

static int action_valid(const struct kb_action *a, unsigned layers)
{
    switch (a->type) {
    case KB_NONE:
    case KB_TRANSPARENT:
        return !a->parameter && !a->code;
    case KB_KEY:
        return !a->parameter && usage(a->code);
    case KB_MODIFIER:
    case KB_ONESHOT:
        return a->parameter && !a->code;
    case KB_MOD_TAP:
        return a->parameter && usage(a->code);
    case KB_MOMENTARY:
    case KB_TOGGLE:
    case KB_LAYER_TAP:
        return a->parameter && a->parameter < layers &&
               (a->type == KB_LAYER_TAP ? usage(a->code) : a->code == 0);
    case KB_CONSUMER:
        return !a->parameter && a->code && a->code <= 0x3ff;
    case KB_MACRO:
        return a->parameter < KB_MACROS && !a->code;
    case KB_GRAVE_ESCAPE:
        return !a->parameter && !a->code;
    case KB_LOCAL:
        return a->parameter <= KB_CAPS_WORD && !a->code;
    default:
        return 0;
    }
}

static int header_valid(unsigned debounce, unsigned tapping, unsigned macro, unsigned layers,
                        unsigned nkro, unsigned rgb, unsigned reserved)
{
    return layers && layers <= KB_LAYERS && nkro <= 1 && rgb < KB_RGB_MODES && !reserved &&
           debounce && debounce <= 50 && tapping >= 20 && tapping <= 1000 && macro && macro <= 1000;
}

int kb_config_validate(const struct kb_config *c)
{
    unsigned l, i;
    if (!c || !header_valid(c->debounce_ms, c->tapping_ms, c->macro_ms, c->layers, c->nkro,
                            c->rgb_mode, c->reserved))
        return -KB_EINVAL;
    for (l = 0; l < KB_LAYERS; ++l)
        for (i = 0; i < KB_KEYS; ++i)
            if (!action_valid(&c->map[l][i], c->layers))
                return -KB_EINVAL;
    for (l = 0; l < KB_MACROS; ++l) {
        if (c->macro_length[l] > KB_MACRO_STEPS)
            return -KB_EINVAL;
        for (i = 0; i < KB_MACRO_STEPS; ++i)
            if (i < c->macro_length[l] ? !usage(c->macros[l][i]) : c->macros[l][i] != 0)
                return -KB_EINVAL;
    }
    return 0;
}

/* Validate the canonical wire image without a second 1 KiB config on stack. */
static int wire_valid(const uint8_t *p)
{
    unsigned i, l, offset = 12, macros = 12 + KB_LAYERS * KB_KEYS * 4;
    if (!header_valid(get16(p), get16(p + 2), get16(p + 4), p[6], p[7], p[9], get16(p + 10)))
        return 0;
    for (i = 0; i < KB_LAYERS * KB_KEYS; ++i, offset += 4) {
        struct kb_action a = {p[offset], p[offset + 1], get16(p + offset + 2)};
        if (!action_valid(&a, p[6]))
            return 0;
    }
    offset = macros + KB_MACROS;
    for (l = 0; l < KB_MACROS; ++l) {
        if (p[macros + l] > KB_MACRO_STEPS)
            return 0;
        for (i = 0; i < KB_MACRO_STEPS; ++i, offset += 2) {
            unsigned code = get16(p + offset);
            if (i < p[macros + l] ? !usage(code) : code != 0)
                return 0;
        }
    }
    return 1;
}

int kb_config_encode(const struct kb_config *c, uint8_t *p, size_t bytes)
{
    unsigned l, i, offset = 12;
    if (!p || bytes != KB_CONFIG_WIRE_BYTES || kb_config_validate(c))
        return -KB_EINVAL;
    put16(p, c->debounce_ms);
    put16(p + 2, c->tapping_ms);
    put16(p + 4, c->macro_ms);
    p[6] = c->layers;
    p[7] = c->nkro;
    p[8] = c->brightness;
    p[9] = c->rgb_mode;
    put16(p + 10, 0);
    for (l = 0; l < KB_LAYERS; ++l)
        for (i = 0; i < KB_KEYS; ++i, offset += 4) {
            p[offset] = c->map[l][i].type;
            p[offset + 1] = c->map[l][i].parameter;
            put16(p + offset + 2, c->map[l][i].code);
        }
    for (l = 0; l < KB_MACROS; ++l)
        p[offset++] = c->macro_length[l];
    for (l = 0; l < KB_MACROS; ++l)
        for (i = 0; i < KB_MACRO_STEPS; ++i, offset += 2)
            put16(p + offset, c->macros[l][i]);
    return 0;
}

static int unpack(struct kb_config *c, const uint8_t *p, size_t bytes)
{
    struct kb_config *candidate = c;
    unsigned l, i, offset = 12;
    if (!c || !p || bytes != KB_CONFIG_WIRE_BYTES)
        return -KB_EINVAL;
    memset(candidate, 0, sizeof(*candidate));
    candidate->debounce_ms = get16(p);
    candidate->tapping_ms = get16(p + 2);
    candidate->macro_ms = get16(p + 4);
    candidate->layers = p[6];
    candidate->nkro = p[7];
    candidate->brightness = p[8];
    candidate->rgb_mode = p[9];
    candidate->reserved = get16(p + 10);
    for (l = 0; l < KB_LAYERS; ++l)
        for (i = 0; i < KB_KEYS; ++i, offset += 4) {
            candidate->map[l][i].type = p[offset];
            candidate->map[l][i].parameter = p[offset + 1];
            candidate->map[l][i].code = get16(p + offset + 2);
        }
    for (l = 0; l < KB_MACROS; ++l)
        candidate->macro_length[l] = p[offset++];
    for (l = 0; l < KB_MACROS; ++l)
        for (i = 0; i < KB_MACRO_STEPS; ++i, offset += 2)
            candidate->macros[l][i] = get16(p + offset);
    return kb_config_validate(candidate);
}

int kb_config_decode(struct kb_config *c, const uint8_t *p, size_t bytes)
{
    struct kb_config candidate;
    if (!c || unpack(&candidate, p, bytes))
        return -KB_EINVAL;
    *c = candidate;
    return 0;
}

uint32_t kb_crc32(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint32_t crc = ~0u;
    unsigned bit;
    while (bytes--) {
        crc ^= *p++;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static int valid_store(const struct kb_store *s, uint32_t base)
{
    return s && s->read && s->write && s->erase && !(base & (KB_CONFIG_SLOT_BYTES - 1)) &&
           base <= UINT32_MAX - 2 * KB_CONFIG_SLOT_BYTES;
}

/* CRC includes extension and length; commit marker is programmed last. */
static uint32_t record_crc(uint8_t *r)
{
    uint32_t old = get32(r + 12), commit = get32(r + 20), crc;
    put32(r + 12, 0);
    put32(r + 20, 0);
    crc = kb_crc32(r + 4, get16(r + 6) + 20u);
    put32(r + 12, old);
    put32(r + 20, commit);
    return crc;
}
static int read_record(const struct kb_store *s, uint32_t offset, uint8_t *r)
{
    if (s->read(s->context, offset, r, 24))
        return -KB_EIO;
    unsigned version = get16(r + 4), size = get16(r + 6);
    if (get32(r) != RECORD_MAGIC || (version != 1 && version != 2) || size < KB_CONFIG_WIRE_BYTES ||
        size > KB_CONFIG_WIRE_BYTES + KB_CONFIG_EXTRA_BYTES ||
        (version == 1 && size != KB_CONFIG_WIRE_BYTES) || get32(r + 16) ||
        get32(r + 20) != RECORD_COMMIT)
        return -KB_EINVAL;
    if (s->read(s->context, offset + 24, r + 24, size))
        return -KB_EIO;
    if (get32(r + 12) != record_crc(r) || !wire_valid(r + 24))
        return -KB_EINVAL;
    return 0;
}
static int newest(const struct kb_store *s, uint32_t base, uint8_t *r, uint32_t *generation)
{
    uint32_t first = 0, second = 0;
    int a = read_record(s, base, r), b;
    if (!a)
        first = get32(r + 8);
    b = read_record(s, base + KB_CONFIG_SLOT_BYTES, r);
    if (!b)
        second = get32(r + 8);
    if (a == -KB_EIO || b == -KB_EIO)
        return -2;
    if (a && b)
        return -1;
    if (!a && (b || (int32_t)(first - second) >= 0)) {
        *generation = first;
        return 0;
    }
    *generation = second;
    return 1;
}
int kb_config_load_extended(const struct kb_store *s, uint32_t base, struct kb_config *c,
                            uint32_t *generation, uint8_t *extra, size_t *extra_size)
{
    uint8_t record[KB_CONFIG_RECORD_MAX_BYTES];
    uint32_t gen;
    if (!c || !generation || !valid_store(s, base) || (!!extra != !!extra_size))
        return -KB_EINVAL;
    int slot = newest(s, base, record, &gen);
    if (slot < 0 || read_record(s, base + (unsigned)slot * KB_CONFIG_SLOT_BYTES, record))
        return -KB_EIO;
    size_t bytes = get16(record + 6) - KB_CONFIG_WIRE_BYTES;
    if (extra_size && bytes > *extra_size)
        return -KB_ENOSPC;
    if (kb_config_decode(c, record + 24, KB_CONFIG_WIRE_BYTES))
        return -KB_EIO;
    if (extra_size) {
        memcpy(extra, record + KB_CONFIG_RECORD_BYTES, bytes);
        *extra_size = bytes;
    }
    *generation = gen;
    return 0;
}
int kb_config_load(const struct kb_store *s, uint32_t base, struct kb_config *c,
                   uint32_t *generation)
{
    return kb_config_load_extended(s, base, c, generation, NULL, NULL);
}
static int save_record(const struct kb_store *s, uint32_t base, const struct kb_config *c,
                       uint32_t *generation, const uint8_t *extra, size_t extra_size, int preserve)
{
    uint8_t record[KB_CONFIG_RECORD_MAX_BYTES], verify[64];
    uint32_t gen = 0, offset;
    if (!generation || !valid_store(s, base) || kb_config_validate(c) ||
        extra_size > KB_CONFIG_EXTRA_BYTES || (!extra && extra_size))
        return -KB_EINVAL;
    int slot = newest(s, base, record, &gen);
    if (slot == -2)
        return -KB_EIO;
    if (preserve && slot >= 0) {
        if (read_record(s, base + (unsigned)slot * KB_CONFIG_SLOT_BYTES, record))
            return -KB_EIO;
        extra_size = get16(record + 6) - KB_CONFIG_WIRE_BYTES;
    } else {
        if (!preserve && extra_size)
            memcpy(record + KB_CONFIG_RECORD_BYTES, extra, extra_size);
        else
            extra_size = 0;
    }
    offset = base + (slot == 0 ? KB_CONFIG_SLOT_BYTES : 0);
    memset(record, 0, 24);
    put32(record, RECORD_MAGIC);
    put16(record + 4, extra_size ? 2 : 1);
    put16(record + 6, KB_CONFIG_WIRE_BYTES + extra_size);
    put32(record + 8, gen + 1);
    kb_config_encode(c, record + 24, KB_CONFIG_WIRE_BYTES);
    put32(record + 12, record_crc(record));
    memset(record + 20, 0xff, 4);
    if (s->erase(s->context, offset, KB_CONFIG_SLOT_BYTES) ||
        s->write(s->context, offset, record, 20) ||
        s->write(s->context, offset + 24, record + 24, KB_CONFIG_WIRE_BYTES + extra_size))
        return -KB_EIO;
    size_t bytes = KB_CONFIG_RECORD_BYTES + extra_size;
    for (size_t checked = 0; checked < bytes;) {
        size_t chunk = bytes - checked;
        if (chunk > sizeof(verify))
            chunk = sizeof(verify);
        if (s->read(s->context, offset + checked, verify, chunk) ||
            memcmp(record + checked, verify, chunk))
            return -KB_EIO;
        checked += chunk;
    }
    put32(record + 20, RECORD_COMMIT);
    if (s->write(s->context, offset + 20, record + 20, 4) || read_record(s, offset, record))
        return -KB_EIO;
    *generation = gen + 1;
    return 0;
}
int kb_config_save_extended(const struct kb_store *s, uint32_t base, const struct kb_config *c,
                            uint32_t *generation, const uint8_t *extra, size_t extra_size)
{
    return save_record(s, base, c, generation, extra, extra_size, 0);
}
int kb_config_save(const struct kb_store *s, uint32_t base, const struct kb_config *c,
                   uint32_t *generation)
{
    return save_record(s, base, c, generation, NULL, 0, 1);
}
