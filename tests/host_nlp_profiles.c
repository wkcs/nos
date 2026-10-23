/* Copyright (C) 2026 NOS contributors. Actual capability state machines with injected
 * device/storage backends. */
#include "nlp/capabilities.h"
#include "nlp/firmware.h"
#include "nlp/hash.h"
#include "nlp/identity.h"
#include "nlp/input_device.h"
#include "nlp/keymap.h"
#include "nlp/radio.h"
#include "nlp/terminal.h"
#include "nlp/text.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
extern const struct nlp_crypto nlp_openssl_crypto;
static int discard(void *context, const uint8_t *p, size_t size)
{
    (void)context;
    (void)p;
    (void)size;
    return 0;
}
static void endpoint(struct nlp_endpoint *e)
{
    struct nlp_endpoint_config c = {.uid = {1},
                                    .fabric = {2},
                                    .boot_id = 1,
                                    .epoch = 1,
                                    .permissions = 511,
                                    .node = 1,
                                    .physical = 1,
                                    .send = discard};
    assert(!nlp_endpoint_init(e, &c));
}
static struct nlp_binding *binding(struct nlp_endpoint *e, unsigned index, unsigned cap)
{
    struct nlp_binding *b = &e->bindings[index];
    memset(b, 0, sizeof(*b));
    b->endpoint = e;
    b->capability = &e->capabilities[cap];
    b->permissions = 511;
    nlp_session_init(&b->session, &e->pool);
    b->session.id = index + 10;
    b->session.epoch = 1;
    b->session.local_node = 1;
    b->session.remote_node = 2;
    b->session.local_cap = b->capability->id;
    b->session.send = discard;
    b->session.message = NULL;
    b->session.active = 1;
    b->session.channel[1].tx_limit = 2;
    return b;
}
static int call(struct nlp_binding *b, unsigned op, const void *in, size_t size, void *out,
                size_t *used)
{
    return b->capability->call(b->capability->context, b, op, in, size, out, used);
}
struct nv {
    uint8_t bytes[8192];
    int budget;
};
static int nv_read(void *context, uint32_t at, void *out, size_t size)
{
    struct nv *n = context;
    assert(at <= 8192 && size <= 8192 - at);
    memcpy(out, n->bytes + at, size);
    return 0;
}
static int nv_write(void *context, uint32_t at, const void *data, size_t size)
{
    struct nv *n = context;
    const uint8_t *p = data;
    assert(at <= 8192 && size <= 8192 - at);
    for (size_t i = 0; i < size; ++i) {
        if (!n->budget)
            return -1;
        if (n->budget > 0)
            --n->budget;
        assert((n->bytes[at + i] & p[i]) == p[i]);
        n->bytes[at + i] &= p[i];
    }
    return 0;
}
static int nv_erase(void *context, uint32_t at, size_t size)
{
    struct nv *n = context;
    assert(!(at % 4096) && size == 4096 && at < 8192);
    memset(n->bytes + at, 255, size);
    return 0;
}
static void identity_test(void)
{
    struct nv n;
    memset(n.bytes, 255, sizeof(n.bytes));
    n.budget = -1;
    uint8_t uid[16] = {7};
    uint64_t boot = 0;
    struct nlp_nv ops = {&n, nv_read, nv_write, nv_erase};
    assert(!nlp_identity_boot(&ops, 0, uid, &boot) && boot == 1);
    assert(!nlp_identity_boot(&ops, 0, uid, &boot) && boot == 2);
    n.budget = 16;
    assert(nlp_identity_boot(&ops, 0, uid, &boot) == -NLP_INTEGRITY && boot == 2);
    n.budget = -1;
    assert(!nlp_identity_boot(&ops, 0, uid, &boot) && boot == 3);
    for (unsigned i = 4; i < 300; ++i)
        assert(!nlp_identity_boot(&ops, 0, uid, &boot) && boot == i);
    uid[1] = 2;
    assert(nlp_identity_boot(&ops, 0, uid, &boot) == -NLP_CONFLICT);
    memset(n.bytes, 255, sizeof(n.bytes));
    n.bytes[0] = 0;
    assert(nlp_identity_boot(&ops, 0, uid, &boot) == -NLP_CONFLICT && !n.bytes[0]);
    puts("PASS nlp_boot_journal_torn_write_rollover_unique_boot_and_unknown_data_preserved");
}
struct config {
    uint8_t bytes[NLP_KEYMAP_BYTES];
    uint32_t generation;
    unsigned commits;
};
static uint32_t config_generation(void *context)
{
    return ((struct config *)context)->generation;
}
static int config_snapshot(void *context, uint8_t *out)
{
    memcpy(out, ((struct config *)context)->bytes, NLP_KEYMAP_BYTES);
    return 0;
}
static int config_validate(void *context, const uint8_t *bytes)
{
    (void)context;
    return bytes[0] == 1 ? 0 : -NLP_INVALID;
}
static int config_commit(void *context, const uint8_t *bytes, int persist, unsigned *state)
{
    struct config *c = context;
    memcpy(c->bytes, bytes, NLP_KEYMAP_BYTES);
    ++c->generation;
    ++c->commits;
    *state = persist ? 1 : 0;
    return 0;
}
static unsigned config_persistence(void *context)
{
    (void)context;
    return 1;
}
static int config_authorized(void *context, const struct nlp_binding *b)
{
    (void)context;
    return !!(b->permissions & 8);
}
static void keymap_test(void)
{
    struct nlp_endpoint e;
    struct nlp_keymap k;
    struct config c = {0};
    endpoint(&e);
    c.bytes[0] = 1;
    struct nlp_keymap_ops ops = {&c,
                                 config_generation,
                                 config_snapshot,
                                 config_validate,
                                 config_commit,
                                 config_persistence,
                                 config_authorized};
    assert(!nlp_keymap_register(&k, &e, 1, &ops));
    struct nlp_binding *b = binding(&e, 1, 0);
    uint8_t begin[40] = {0}, data[NLP_KEYMAP_BYTES], chunk[120], out[128];
    size_t used = 128;
    memset(data, 7, sizeof(data));
    data[0] = 1;
    nlp_put32(begin + 4, NLP_KEYMAP_BYTES);
    assert(!nlp_sha256(data, sizeof(data), begin + 8));
    b->permissions = 1;
    assert(call(b, 2, begin, 40, out, &used) == -NLP_DENIED);
    b->permissions = 9;
    used = 128;
    assert(!call(b, 2, begin, 40, out, &used) && used == 8);
    uint64_t id = nlp_get64(out);
    for (unsigned offset = 0; offset < NLP_KEYMAP_BYTES;) {
        unsigned size = NLP_KEYMAP_BYTES - offset;
        if (size > 104)
            size = 104;
        nlp_put64(chunk, id);
        nlp_put32(chunk + 8, offset);
        nlp_put16(chunk + 12, (uint16_t)size);
        nlp_put16(chunk + 14, 0);
        memcpy(chunk + 16, data + offset, size);
        used = 128;
        assert(!call(b, 4, chunk, 16 + size, out, &used));
        offset += size;
        assert(nlp_get32(out) == offset);
    }
    uint8_t commit[16] = {0};
    nlp_put64(commit, id);
    commit[12] = 1;
    used = 128;
    assert(!call(b, 6, commit, 16, out, &used));
    assert(c.commits == 1 && c.generation == 1 && out[4] == 1 &&
           !memcmp(c.bytes, data, sizeof(data)));
    used = 128;
    assert(!call(b, 6, commit, 16, out, &used) && c.commits == 1);
    used = 128;
    assert(call(b, 7, commit, 8, out, &used) == -NLP_CONFLICT);
    begin[0] = 0;
    used = 128;
    assert(call(b, 2, begin, 40, out, &used) == -NLP_CONFLICT);
    nlp_put32(begin, 1);
    used = 128;
    assert(!call(b, 2, begin, 40, out, &used));
    id = nlp_get64(out);
    nlp_put64(chunk, id);
    nlp_put32(chunk + 8, 0);
    nlp_put16(chunk + 12, 1);
    nlp_put16(chunk + 14, 0);
    chunk[16] = 2;
    used = 128;
    assert(!call(b, 4, chunk, 17, out, &used));
    chunk[16] = 3;
    used = 128;
    assert(call(b, 4, chunk, 17, out, &used) == -NLP_CONFLICT);
    e.capabilities[0].disconnect(&k, b->session.id);
    assert(!k.active);
    puts("PASS nlp_keymap_sha256_generation_atomic_commit_idempotency_permission_and_disconnect");
}
struct panel {
    unsigned begins, tiles, ends, aborts;
    uint8_t pixels[4];
};
static int panel_begin(void *p, unsigned atomic)
{
    assert(atomic == 1);
    ++((struct panel *)p)->begins;
    return 0;
}
static int panel_tile(void *p, uint16_t x, uint16_t y, uint16_t w, uint16_t h, unsigned format,
                      const uint8_t *bytes, size_t size)
{
    struct panel *panel = p;
    assert(!x && !y && w == 2 && h == 1 && !format && size == 4);
    memcpy(panel->pixels, bytes, 4);
    ++panel->tiles;
    return 0;
}
static int panel_end(void *p)
{
    ++((struct panel *)p)->ends;
    return 0;
}
static void panel_abort(void *p)
{
    ++((struct panel *)p)->aborts;
}
static int panel_power(void *p, int enabled)
{
    (void)p;
    (void)enabled;
    return 0;
}
static void display_test(void)
{
    struct nlp_endpoint e;
    struct nlp_display d;
    struct panel p = {0};
    endpoint(&e);
    struct nlp_display_ops ops = {
        &p, 2, 1, 1, 3, 128, 4, 10, panel_begin, panel_tile, panel_end, panel_abort, panel_power};
    assert(!nlp_display_register(&d, &e, 1, &ops));
    struct nlp_binding *b = binding(&e, 1, 0);
    uint8_t in[22] = {0}, out[128];
    size_t used = 128;
    nlp_put32(in, 1);
    nlp_put16(in + 4, 1);
    in[6] = 1;
    assert(!call(b, 2, in, 8, out, &used));
    struct nlp_binding *other = binding(&e, 2, 0);
    uint8_t end[8];
    nlp_put32(end, 1);
    used = 128;
    assert(call(other, 4, end, 4, out, &used) == -NLP_STALE);
    memset(in, 0, 22);
    nlp_put32(in, 1);
    nlp_put16(in + 10, 2);
    nlp_put16(in + 12, 1);
    nlp_put16(in + 16, 4);
    memcpy(in + 18, "RGBA", 4);
    used = 0;
    assert(!call(b, 0x2001, in, 22, NULL, &used));
    assert(p.tiles == 1 && !p.ends);
    nlp_put32(end + 4, nlp_crc32c(in, 22) ^ 1);
    used = 128;
    assert(call(b, 3, end, 8, out, &used) == -NLP_INTEGRITY && !p.ends);
    nlp_put32(end + 4, nlp_crc32c(in, 22));
    used = 128;
    assert(!call(b, 3, end, 8, out, &used) && p.ends == 1);
    memset(in, 0, 8);
    nlp_put32(in, 2);
    nlp_put16(in + 4, 1);
    in[6] = 1;
    used = 128;
    assert(call(b, 2, in, 8, out, &used) == -NLP_BUSY);
    e.now_ms = 10;
    used = 128;
    assert(!call(b, 2, in, 8, out, &used));
    e.capabilities[0].disconnect(&d, b->session.id);
    assert(p.aborts == 1 && !d.active);
    puts("PASS nlp_display_tile_bounds_crc_atomic_present_owner_interval_and_disconnect_abort");
}
struct terminal {
    unsigned opens, writes, closed, signaled;
    uint8_t input[32];
};
static int term_open(void *p, uint16_t cols, uint16_t rows, const char *name)
{
    assert(cols == 80 && rows == 24 && !strcmp(name, "xterm"));
    ++((struct terminal *)p)->opens;
    return 0;
}
static int term_resize(void *p, uint16_t cols, uint16_t rows)
{
    (void)p;
    assert(cols && rows);
    return 0;
}
static int term_interrupt(void *p, unsigned signal)
{
    assert(signal == 2 || signal == 15);
    ++((struct terminal *)p)->signaled;
    return 0;
}
static int term_write(void *p, const uint8_t *bytes, size_t size)
{
    struct terminal *t = p;
    assert(size <= 32);
    memcpy(t->input, bytes, size);
    ++t->writes;
    return 0;
}
static int term_read(void *p, uint8_t *bytes, size_t limit, size_t *size)
{
    (void)p;
    (void)bytes;
    (void)limit;
    *size = 0;
    return -NLP_BUSY;
}
static int term_exit(void *p, int32_t *status)
{
    (void)p;
    *status = 0;
    return 0;
}
static void term_close(void *p)
{
    ++((struct terminal *)p)->closed;
}
static void terminal_test(void)
{
    struct nlp_endpoint e;
    struct nlp_terminal t;
    struct terminal backend = {0};
    endpoint(&e);
    struct nlp_terminal_ops ops = {&backend,   term_open, term_resize, term_interrupt,
                                   term_write, term_read, term_exit,   term_close};
    assert(!nlp_terminal_register(&t, &e, 1, &ops));
    struct nlp_binding *b = binding(&e, 1, 0);
    uint8_t in[32] = {0}, out[128];
    size_t used = 128;
    nlp_put16(in, 80);
    nlp_put16(in + 2, 24);
    nlp_put16(in + 6, 5);
    memcpy(in + 8, "xterm", 5);
    b->permissions = 0;
    assert(call(b, 1, in, 13, out, &used) == -NLP_DENIED);
    b->permissions = 32;
    used = 128;
    assert(!call(b, 1, in, 13, out, &used) && used == 12);
    uint32_t id = nlp_get32(out);
    nlp_put32(in, id);
    memcpy(in + 4, "echo\n", 5);
    used = 0;
    assert(!call(b, 0x2001, in, 9, NULL, &used) && backend.writes == 1);
    nlp_put16(in + 4, 2);
    nlp_put16(in + 6, 0);
    used = 128;
    assert(!call(b, 3, in, 8, out, &used) && backend.signaled == 1);
    nlp_put16(in + 4, 9);
    used = 128;
    assert(call(b, 3, in, 8, out, &used) == -NLP_INVALID && backend.signaled == 1);
    e.capabilities[0].disconnect(&t, b->session.id);
    assert(backend.closed == 1 && !t.active);
    puts("PASS nlp_terminal_permission_real_bytes_interrupt_bounds_and_owner_disconnect_cleanup");
}
struct storage {
    struct nlp_update saved;
    uint8_t image[64], public_key[32];
    uint64_t next_id, applied;
    unsigned prepares, writes, applies, confirms;
};
static int fw_allocate(void *p, uint64_t *id)
{
    *id = ++((struct storage *)p)->next_id;
    return 0;
}
static int fw_load(void *p, struct nlp_update *u)
{
    struct storage *s = p;
    if (!s->saved.state)
        return -NLP_NOT_FOUND;
    *u = s->saved;
    return 0;
}
static int fw_checkpoint(void *p, const struct nlp_update *u)
{
    ((struct storage *)p)->saved = *u;
    return 0;
}
static int fw_signature(void *p, uint32_t key, const uint8_t *bytes, size_t size,
                        const uint8_t sig[64])
{
    struct storage *s = p;
    if (key != 7)
        return -NLP_DENIED;
    EVP_PKEY *public_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, s->public_key, 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int good = public_key && ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, public_key) == 1 &&
               EVP_DigestVerify(ctx, sig, 64, bytes, size) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(public_key);
    return good ? 0 : -NLP_INTEGRITY;
}
static int fw_prepare(void *p, uint32_t size)
{
    assert(size == 64);
    struct storage *s = p;
    memset(s->image, 255, 64);
    ++s->prepares;
    return 0;
}
static int fw_read(void *p, uint32_t offset, void *out, size_t size)
{
    assert(offset <= 64 && size <= 64 - offset);
    memcpy(out, ((struct storage *)p)->image + offset, size);
    return 0;
}
static int fw_write(void *p, uint32_t offset, const void *in, size_t size)
{
    struct storage *s = p;
    assert(offset <= 64 && size <= 64 - offset);
    memcpy(s->image + offset, in, size);
    ++s->writes;
    return 0;
}
static int fw_recover(void *p, uint32_t offset)
{
    assert(offset <= 64 && !(offset % 32));
    memset(((struct storage *)p)->image + offset, 255, 64 - offset);
    return 0;
}
static int fw_verify(void *p, uint32_t size, const uint8_t sha[32])
{
    uint8_t digest[32];
    assert(size == 64);
    assert(!nlp_sha256(((struct storage *)p)->image, 64, digest));
    return memcmp(digest, sha, 32) ? -NLP_INTEGRITY : 0;
}
static int fw_abort(void *p, uint64_t id)
{
    (void)id;
    memset(((struct storage *)p)->image, 255, 64);
    return 0;
}
static int fw_apply(void *p, uint64_t id)
{
    struct storage *s = p;
    if (s->applied != id) {
        s->applied = id;
        ++s->applies;
    }
    return 0;
}
static int fw_confirm(void *p, uint64_t id)
{
    struct storage *s = p;
    assert(s->applied == id);
    ++s->confirms;
    return 0;
}
static void firmware_test(void)
{
    struct nlp_endpoint e;
    struct nlp_firmware f;
    struct storage backend = {0};
    endpoint(&e);
    struct nlp_firmware_ops ops = {.context = &backend,
                                   .crypto = &nlp_openssl_crypto,
                                   .board = {1},
                                   .component = {2},
                                   .hardware = 3,
                                   .image_limit = 64,
                                   .erase_granule = 32,
                                   .allocate_id = fw_allocate,
                                   .load = fw_load,
                                   .checkpoint = fw_checkpoint,
                                   .signature = fw_signature,
                                   .prepare = fw_prepare,
                                   .read = fw_read,
                                   .write = fw_write,
                                   .recover = fw_recover,
                                   .verify_image = fw_verify,
                                   .abort = fw_abort};
    uint8_t manifest[512], value[64], canonical[519], seed[32], image[64], out[128], begin[52],
        chunk[120];
    size_t length = 0, used = 128;
    memset(seed, 77, 32);
    for (unsigned i = 0; i < 64; ++i)
        image[i] = (uint8_t)i;
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    size_t pub_size = 32;
    assert(key && EVP_PKEY_get_raw_public_key(key, backend.public_key, &pub_size));
#define ADD(tag, data, size)                                                                       \
    assert(!nlp_tlv_add(manifest, sizeof(manifest), &length, tag, data, size))
    ADD(1, ops.board, 16);
    nlp_put32(value, 3);
    ADD(2, value, 4);
    ADD(3, value, 4);
    ADD(4, ops.component, 16);
    nlp_put32(value, 64);
    ADD(5, value, 4);
    assert(!nlp_sha256(image, 64, value));
    ADD(6, value, 32);
    nlp_put64(value, 1);
    ADD(7, value, 8);
    ADD(8, value, 8);
    nlp_put32(value, 7);
    ADD(9, value, 4);
    memcpy(canonical, "NLP-FW1", 7);
    memcpy(canonical + 7, manifest, length);
    EVP_MD_CTX *sign = EVP_MD_CTX_new();
    size_t sig_size = 64;
    assert(sign && EVP_DigestSignInit(sign, NULL, NULL, NULL, key) == 1 &&
           EVP_DigestSign(sign, value, &sig_size, canonical, length + 7) == 1 && sig_size == 64);
    EVP_MD_CTX_free(sign);
    EVP_PKEY_free(key);
    ADD(10, value, 64);
#undef ADD
    assert(!nlp_firmware_register(&f, &e, 1, &ops));
    struct nlp_binding *b = binding(&e, 1, 0);
    memcpy(begin, ops.component, 16);
    nlp_put16(begin + 16, (uint16_t)length);
    nlp_put16(begin + 18, 1);
    assert(!nlp_sha256(manifest, length, begin + 20));
    used = 128;
    assert(!call(b, 2, begin, 52, out, &used));
    uint64_t id = nlp_get64(out);
    assert(id && backend.prepares == 0);
    for (size_t offset = 0; offset < length;) {
        size_t count = length - offset;
        if (count > 104)
            count = 104;
        nlp_put64(chunk, id);
        nlp_put32(chunk + 8, (uint32_t)offset);
        nlp_put16(chunk + 12, (uint16_t)count);
        nlp_put16(chunk + 14, 0);
        memcpy(chunk + 16, manifest + offset, count);
        used = 128;
        assert(!call(b, 3, chunk, count + 16, out, &used));
        offset += count;
    }
    assert(backend.prepares == 1 && f.update.state == NLP_UPDATE_RECEIVING);
    nlp_put64(chunk, id);
    nlp_put32(chunk + 8, 0);
    nlp_put16(chunk + 12, 16);
    nlp_put16(chunk + 14, 0);
    memcpy(chunk + 16, image, 16);
    used = 128;
    assert(!call(b, 4, chunk, 32, out, &used));
    assert(nlp_get32(out) == 16 && !nlp_get32(out + 4));
    /* Power loss: only durable offset survives; uncommitted owned staging sector is rewound. */
    struct nlp_endpoint resumed;
    struct nlp_firmware recovered;
    endpoint(&resumed);
    assert(!nlp_firmware_register(&recovered, &resumed, 1, &ops));
    b = binding(&resumed, 1, 0);
    assert(!recovered.update.accepted && !recovered.update.durable);
    for (unsigned offset = 0; offset < 64; offset += 32) {
        nlp_put64(chunk, id);
        nlp_put32(chunk + 8, offset);
        nlp_put16(chunk + 12, 32);
        memcpy(chunk + 16, image + offset, 32);
        used = 128;
        assert(!call(b, 4, chunk, 48, out, &used));
    }
    unsigned writes = backend.writes;
    used = 128;
    assert(!call(b, 4, chunk, 48, out, &used) && backend.writes == writes);
    chunk[16] ^= 1;
    used = 128;
    assert(call(b, 4, chunk, 48, out, &used) == -NLP_CONFLICT);
    chunk[16] ^= 1;
    nlp_put64(chunk, id);
    used = 128;
    assert(!call(b, 6, chunk, 8, out, &used) && recovered.update.state == NLP_UPDATE_VERIFIED);
    used = 128;
    assert(call(b, 7, chunk, 8, out, &used) == -NLP_UNSUPPORTED && !backend.applies);
    recovered.ops.mode = 1;
    recovered.ops.apply = fw_apply;
    recovered.ops.confirm = fw_confirm;
    b->permissions = 64;
    used = 128;
    assert(call(b, 7, chunk, 8, out, &used) == -NLP_DENIED && !backend.applies);
    b->permissions = 64 | 128;
    used = 128;
    assert(!call(b, 7, chunk, 8, out, &used));
    used = 128;
    assert(!call(b, 7, chunk, 8, out, &used) && backend.applies == 1);
    used = 128;
    assert(call(b, 8, chunk, 8, out, &used) == -NLP_CONFLICT);
    used = 128;
    assert(!call(b, 9, chunk, 8, out, &used) && recovered.update.state == NLP_UPDATE_COMMITTED &&
           backend.confirms == 1);
    puts("PASS "
         "nlp_firmware_real_ed25519_sha256_manifest_resume_durable_offset_duplicate_write_apply_"
         "gate_and_idempotency");
}
static unsigned radio_calls, cancels;
static int radio_control(void *context, unsigned op, const struct nlp_radio_params *p)
{
    (void)context;
    assert(op == 2 && p->duration_ms == 100 && p->max_results == 2);
    ++radio_calls;
    return 0;
}
static void radio_cancel(void *context)
{
    (void)context;
    ++cancels;
}
static void radio_text_test(void)
{
    struct nlp_endpoint e;
    struct nlp_radio radio;
    endpoint(&e);
    assert(!nlp_radio_register(&radio, &e, 1, 0, 1, radio_control, radio_cancel, NULL));
    struct nlp_binding *b = binding(&e, 1, 0);
    uint8_t in[128], out[128], value[4];
    size_t size = 0, used = 128;
    nlp_put32(value, 1);
    assert(!nlp_tlv_add(in, 128, &size, 1, value, 4));
    nlp_put32(value, 100);
    assert(!nlp_tlv_add(in, 128, &size, 3, value, 4));
    nlp_put16(value, 2);
    assert(!nlp_tlv_add(in, 128, &size, 4, value, 2));
    assert(call(b, 2, in, size, out, &used) == -NLP_DENIED && !radio_calls);
    struct nlp_cipher cipher = {.crypto = &nlp_openssl_crypto};
    b->session.channel[1].cipher = &cipher;
    used = 128;
    assert(!call(b, 2, in, size, out, &used) && radio_calls == 1);
    used = 128;
    assert(call(b, 2, in, size, out, &used) == -NLP_BUSY && radio_calls == 1);
    size = 0;
    assert(!nlp_tlv_add(in, 128, &size, 13, "secretkey", 9));
    assert(nlp_radio_event(&radio, 0x1002, in, size) == -NLP_DENIED);
    e.capabilities[0].disconnect(&radio, b->session.id);
    assert(cancels == 1 && !radio.job);
    size_t prefix;
    assert(!nlp_utf8_prefix((const uint8_t *)"日志", 6, 4, &prefix) && prefix == 3);
    const uint8_t invalid[] = {0xed, 0xa0, 0x80};
    assert(nlp_utf8_prefix(invalid, 3, 3, &prefix) == -NLP_INVALID);
    assert(!nlp_utf8_prefix(NULL, 0, 0, &prefix) && !prefix);
    puts(
        "PASS "
        "nlp_radio_encrypted_authorization_bounded_async_job_secret_redaction_and_utf8_boundaries");
}
void nlp_profile_tests(void)
{
    identity_test();
    keymap_test();
    display_test();
    terminal_test();
    firmware_test();
    radio_text_test();
}
