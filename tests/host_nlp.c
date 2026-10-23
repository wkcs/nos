/* Copyright (C) 2026 NOS contributors. Actual NLP C core, not a parallel protocol model. */
#include "nlp/capabilities.h"
#include "nlp/router.h"
#include "nlp/hash.h"
void nlp_profile_tests(void);
void nlp_fabric_tests(void);
void nlp_extra_tests(void);
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const struct nlp_crypto nlp_openssl_crypto;
static void hex(const char *name, const uint8_t *p, size_t size)
{
    printf("%s ", name);
    for (size_t i = 0; i < size; ++i)
        printf("%02x", p[i]);
    putchar('\n');
}
static int pin(void *context, const uint8_t key[32])
{
    return memcmp(context, key, 32) ? -NLP_DENIED : 0;
}
static unsigned random_call;
static int deterministic_random(uint8_t *out, size_t size)
{
    memset(out, ++random_call, size);
    return 0;
}
static void noise_test(void)
{
    struct nlp_noise a, b;
    struct nlp_crypto crypto = nlp_openssl_crypto;
    uint8_t sa[32], sb[32], pa[32], pb[32], first[96], second[96], third[96];
    size_t one, two, three;
    memset(sa, 33, 32);
    memset(sb, 44, 32);
    crypto.random = deterministic_random;
    assert(!crypto.public_key(sa, pa) && !crypto.public_key(sb, pb));
    assert(!nlp_noise_init(&a, &crypto, 1, sa, (const uint8_t *)"nlp-test", 8, pin, pb));
    assert(!nlp_noise_init(&b, &crypto, 0, sb, (const uint8_t *)"nlp-test", 8, pin, pa));
    assert(!nlp_noise_write(&a, first, &one) && one == 32);
    assert(!nlp_noise_read(&b, first, one));
    assert(!nlp_noise_write(&b, second, &two) && two == 96);
    assert(!nlp_noise_read(&a, second, two));
    assert(!nlp_noise_write(&a, third, &three) && three == 64);
    assert(!nlp_noise_read(&b, third, three) && a.complete && b.complete);
    assert(!memcmp(a.tx, b.rx, 32) && !memcmp(a.rx, b.tx, 32) && !memcmp(a.hash, b.hash, 32));
    hex("noise_msg1", first, one);
    hex("noise_msg2", second, two);
    hex("noise_msg3", third, three);
    hex("noise_hash", a.hash, 32);
    hex("noise_tx", a.tx, 32);
    hex("noise_rx", a.rx, 32);
    struct nlp_cipher ca, cb;
    const uint8_t tx[] = "NLP1/channel/direction-a", rx[] = "NLP1/channel/direction-b";
    assert(!nlp_cipher_derive(&ca, &a, tx, sizeof(tx) - 1, rx, sizeof(rx) - 1));
    assert(!nlp_cipher_derive(&cb, &b, rx, sizeof(rx) - 1, tx, sizeof(tx) - 1));
    struct nlp_header h = {.kind = NLP_DATA,
                           .flags = NLP_ENCRYPTED,
                           .priority = 1,
                           .hop_limit = 16,
                           .fabric_epoch = 1,
                           .src_node = 1,
                           .dst_node = 2,
                           .src_cap = 3,
                           .dst_cap = 0,
                           .channel_id = 1,
                           .opcode = 0x2001,
                           .session_id = 4,
                           .packet_counter = 1};
    uint8_t frame[NLP_PDU_MAX], plaintext[NLP_PAYLOAD_MAX];
    size_t size;
    struct nlp_frame decoded;
    assert(!nlp_cipher_encode(&ca, &h, "secret", 6, frame, sizeof(frame), &size));
    assert(nlp_frame_decode(frame, size, &decoded) == -NLP_UNSUPPORTED);
    assert(!nlp_cipher_decode(&cb, frame, size, &decoded, plaintext) &&
           !memcmp(plaintext, "secret", 6));
    frame[7] = 2;
    nlp_put32(frame + 48, nlp_crc32c(frame, 48));
    assert(!nlp_cipher_decode(&cb, frame, size, &decoded, plaintext));
    frame[12] = 3;
    nlp_put32(frame + 48, nlp_crc32c(frame, 48));
    assert(nlp_cipher_decode(&cb, frame, size, &decoded, plaintext) == -NLP_INTEGRITY);
    frame[12] = 1;
    nlp_put32(frame + 48, nlp_crc32c(frame, 48));
    frame[size - 1] ^= 1;
    assert(nlp_cipher_decode(&cb, frame, size, &decoded, plaintext) == -NLP_INTEGRITY);
    memset(pa, 0, 32);
    random_call = 0;
    assert(!nlp_noise_init(&a, &crypto, 1, sa, (const uint8_t *)"nlp-test", 8, pin, pa));
    assert(!nlp_noise_write(&a, first, &one));
    assert(nlp_noise_read(&a, second, two) == -NLP_DENIED && a.failed);
    for (unsigned i = 0; i < 32; ++i)
        assert(!a.s[i] && !a.e[i]);
    puts("PASS nlp_noise_xx_pinned_keys_aead_tamper_and_no_plaintext_downgrade");
}
struct packet {
    uint8_t bytes[NLP_PDU_MAX];
    size_t size;
};
struct peer {
    struct nlp_session session;
    struct nlp_pool pool;
    struct packet queue[32];
    unsigned count, delivered, sum;
    int busy;
};
static int enqueue(void *context, const uint8_t *p, size_t size)
{
    struct peer *peer = context;
    if (peer->count == 32)
        return -NLP_BUSY;
    memcpy(peer->queue[peer->count].bytes, p, size);
    peer->queue[peer->count++].size = size;
    return 0;
}
static int message(void *context, struct nlp_session *session, const struct nlp_frame *f)
{
    struct peer *p = context;
    (void)session;
    if (p->busy)
        return -NLP_BUSY;
    assert(f->length == 1);
    ++p->delivered;
    p->sum += f->payload[0];
    return 0;
}
static void initialize(struct peer *p, unsigned node)
{
    memset(p, 0, sizeof(*p));
    nlp_session_init(&p->session, &p->pool);
    p->session.id = 42;
    p->session.epoch = 1;
    p->session.local_node = (uint16_t)node;
    p->session.remote_node = (uint16_t)(3 - node);
    p->session.window = 2;
    p->session.send = enqueue;
    p->session.message = message;
    p->session.context = p;
    assert(!nlp_session_start(&p->session, 0));
}
static void transfer(struct peer *from, struct peer *to, uint32_t now)
{
    unsigned count = from->count;
    from->count = 0;
    for (unsigned i = 0; i < count; ++i)
        assert(!nlp_session_receive(&to->session, from->queue[i].bytes, from->queue[i].size, now));
}
static void session_test(void)
{
    struct peer a, b;
    initialize(&a, 1);
    initialize(&b, 2);
    uint8_t value = 7;
    assert(nlp_session_send(&a.session, 1, NLP_DATA, 1, 1, &value, 1, 0) == -NLP_BUSY);
    nlp_session_poll(&a.session, 0);
    nlp_session_poll(&b.session, 0);
    transfer(&a, &b, 0);
    transfer(&b, &a, 0);
    assert(a.session.channel[1].tx_limit == 2 && b.session.channel[1].tx_limit == 2);
    assert(!nlp_session_send(&a.session, 1, NLP_DATA, 1, 1, &value, 1, 1));
    value = 9;
    assert(!nlp_session_send(&a.session, 1, NLP_DATA, 1, 1, &value, 1, 1));
    assert(nlp_session_send(&a.session, 1, NLP_DATA, 1, 1, &value, 1, 1) == -NLP_BUSY);
    nlp_session_poll(&a.session, 1);
    assert(a.count == 2);
    assert(!nlp_session_receive(&b.session, a.queue[1].bytes, a.queue[1].size, 1));
    nlp_session_poll(&b.session, 1);
    assert(!b.delivered);
    assert(!nlp_session_receive(&b.session, a.queue[0].bytes, a.queue[0].size, 2));
    assert(!nlp_session_receive(&b.session, a.queue[0].bytes, a.queue[0].size, 2));
    nlp_session_poll(&b.session, 2);
    nlp_session_poll(&b.session, 3);
    assert(b.delivered == 2 && b.sum == 16);
    a.count = 0;
    transfer(&b, &a, 3);
    assert(a.session.channel[1].tx_acked == 2);
    value = 3;
    assert(!nlp_session_send(&a.session, 1, NLP_DATA, 1, 1, &value, 1, 4));
    nlp_session_poll(&a.session, 4);
    assert(a.count == 1);
    struct packet original = a.queue[0];
    a.count = 0;
    nlp_session_poll(&a.session, 105);
    assert(a.count == 1);
    assert(original.size == a.queue[0].size &&
           !memcmp(original.bytes, a.queue[0].bytes, original.size));
    b.busy = 1;
    transfer(&a, &b, 105);
    nlp_session_poll(&b.session, 105);
    uint32_t limit = b.session.channel[1].rx_limit;
    assert(b.delivered == 2);
    nlp_session_poll(&b.session, 106);
    assert(b.session.channel[1].rx_limit == limit);
    b.busy = 0;
    nlp_session_poll(&b.session, 107);
    assert(b.delivered == 3);
    nlp_session_close(&b.session, NLP_DISCONNECTED);
    for (unsigned i = 0; i < NLP_POOL_SLOTS; ++i)
        assert(!b.pool.slot[i].owner);
    nlp_session_poll(&a.session, 16000);
    assert(!a.session.active);
    puts("PASS nlp_reliable_credit_order_dedupe_identical_retry_backpressure_and_revocation");
}
static int got(void *context, const uint8_t *bytes, size_t size)
{
    struct packet *p = context;
    memcpy(p->bytes, bytes, size);
    p->size = size;
    return 0;
}
static void transport_test(void)
{
    struct nlp_header h = {.kind = 3,
                           .priority = 1,
                           .hop_limit = 16,
                           .fabric_epoch = 1,
                           .src_node = 1,
                           .dst_node = 2,
                           .session_id = 1,
                           .packet_counter = 1};
    uint8_t data[NLP_PDU_MAX], framed[NLP_ENCODED_MAX], fragment[64], status[20],
        peek[NLP_PDU_MAX + 14];
    size_t size, framed_size, consumed, part;
    struct packet result = {0};
    assert(!nlp_frame_encode(&h, "test", 4, data, sizeof(data), &size));
    for (unsigned mode = 0; mode < 2; ++mode) {
        assert(!nlp_stream_wrap(mode, data, size, framed, sizeof(framed), &framed_size));
        for (size_t split = 0; split <= framed_size; ++split) {
            struct nlp_stream stream;
            nlp_stream_init(&stream, mode);
            result.size = 0;
            assert(!nlp_stream_feed(&stream, framed, split, &consumed, 0, got, &result) &&
                   consumed == split);
            assert(!nlp_stream_feed(&stream, framed + split, framed_size - split, &consumed, 1, got,
                                    &result));
            assert(result.size == size && !memcmp(result.bytes, data, size));
        }
    }
    struct nlp_mailbox m;
    nlp_mailbox_init(&m, 1);
    assert(!nlp_mailbox_enqueue(&m, data, size));
    assert(!nlp_mailbox_read(&m, 0x10, status, 20));
    assert(nlp_get16(status + 6) == size);
    assert(!nlp_mailbox_read(&m, 0x30, peek, size + 14));
    assert(!memcmp(peek + 14, data, size));
    assert(!nlp_mailbox_write(&m, 0x40, peek, 12, NULL, NULL));
    assert(!m.count);
    assert(!nlp_mailbox_enqueue(&m, data, size));
    assert(!nlp_mailbox_write(&m, 0x40, peek, 12, NULL, NULL));
    assert(m.count == 1);
    nlp_mailbox_init(&m, 2);
    assert(!nlp_mailbox_enqueue(&m, data, size));
    assert(!nlp_mailbox_write(&m, 0x40, peek, 12, NULL, NULL));
    assert(m.count == 1);
    for (unsigned mtu = 8; mtu <= 64; mtu += 56) {
        struct nlp_can c = {0};
        uint32_t id;
        nlp_can_reset(&c, 2);
        result.size = 0;
        assert(!nlp_can_id(1, 1, 2, 0, 0, &id));
        for (unsigned index = 0;; ++index) {
            assert(!nlp_can_fragment(data, size, mtu, index, fragment, &part));
            assert(!nlp_can_receive(&c, id, fragment, part, index, got, &result));
            if (fragment[1] & 2)
                break;
            assert(!nlp_can_receive(&c, id, fragment, part, index, got, &result));
        }
        assert(result.size == size && !memcmp(result.bytes, data, size));
    }
    puts("PASS nlp_stream_split_mailbox_stale_boot_token_and_can_classic_fd_actual_codec");
}
static void router_input_test(void)
{
    struct nlp_router root;
    uint8_t uid[16] = {1}, fabric[16] = {2}, child[16] = {3};
    nlp_router_init(&root, uid);
    assert(!nlp_router_join(&root, fabric, 1, 1, NLP_NO_PORT, NULL, 0));
    assert(!nlp_router_prepare(&root, 2, child, 0, 1, 0, 15000));
    struct nlp_header h = {.kind = 3,
                           .priority = 1,
                           .hop_limit = 16,
                           .fabric_epoch = 1,
                           .src_node = 1,
                           .dst_node = 2,
                           .session_id = 1,
                           .packet_counter = 1};
    uint8_t raw[NLP_PDU_MAX], egress;
    size_t size;
    assert(!nlp_frame_encode(&h, NULL, 0, raw, sizeof(raw), &size));
    assert(nlp_router_forward(&root, NLP_NO_PORT, raw, size, &egress) == -NLP_NO_ROUTE);
    assert(!nlp_router_commit(&root, 1));
    assert(!nlp_router_forward(&root, NLP_NO_PORT, raw, size, &egress) && egress == 0 &&
           raw[7] == 16);
    assert(nlp_router_forward(&root, 1, raw, size, &egress) == -NLP_DENIED);
    nlp_router_expire(&root, 15000);
    assert(nlp_router_forward(&root, NLP_NO_PORT, raw, size, &egress) == -NLP_NO_ROUTE);
    struct nlp_inputs inputs = {0};
    uint16_t usages[16] = {4};
    uint8_t uid2[16] = {4}, state[16] = {0}, report[33] = {0};
    assert(!nlp_input_bind(&inputs, uid, 1, 1, 1, 1, usages, 300));
    assert(!nlp_input_bind(&inputs, uid2, 1, 1, 2, 1, usages, 300));
    nlp_put32(state, 1);
    assert(!nlp_input_state(&inputs, uid, 1, 1, state, 0));
    assert(!nlp_input_state(&inputs, uid2, 1, 1, state, 0));
    nlp_put32(state, 2);
    nlp_put16(state + 12, 1);
    assert(!nlp_input_state(&inputs, uid, 1, 1, state, 1));
    assert(!nlp_input_state(&inputs, uid2, 1, 1, state, 1));
    nlp_input_drop(&inputs, uid, 1);
    nlp_input_merge(&inputs, 2, report);
    assert(report[1] & 16);
    memset(report, 0, 33);
    nlp_input_merge(&inputs, 301, report);
    assert(!report[1]);
    nlp_put32(state, 3);
    assert(!nlp_input_state(&inputs, uid2, 1, 1, state, 302));
    nlp_input_merge(&inputs, 303, report);
    assert(!report[1]);
    puts(
        "PASS nlp_tree_prepare_commit_source_filter_expiry_and_input_source_release_lease_barrier");
}
static int vector(const char *input)
{
    uint8_t raw[NLP_PDU_MAX], encoded[NLP_PDU_MAX], uart[NLP_ENCODED_MAX];
    struct nlp_frame frame;
    size_t size = strlen(input) / 2, written;
    if (strlen(input) % 2 || size > sizeof(raw))
        return 1;
    for (size_t i = 0; i < size; ++i) {
        unsigned byte;
        if (sscanf(input + i * 2, "%2x", &byte) != 1)
            return 1;
        raw[i] = (uint8_t)byte;
    }
    if (nlp_frame_decode(raw, size, &frame))
        return 1;
    if (nlp_frame_encode(&frame.h, frame.payload, frame.length, encoded, sizeof(encoded),
                         &written) ||
        written != size || memcmp(raw, encoded, size))
        return 1;
    if (nlp_stream_wrap(1, raw, size, uart, sizeof(uart), &written))
        return 1;
    hex("uart", uart, written);
    return 0;
}
static int hash_vector(const char *input)
{
    uint8_t raw[2048], digest[32]; size_t size = strlen(input) / 2;
    if (strlen(input) % 2 || size > sizeof(raw)) return 1;
    for (size_t i = 0; i < size; ++i) {
        unsigned byte; if (sscanf(input + i * 2, "%2x", &byte) != 1) return 1;
        raw[i] = (uint8_t)byte;
    }
    if (nlp_sha256(raw, size, digest)) return 1;
    hex("sha256", digest, 32); return 0;
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "sha256")) return hash_vector(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "wire"))
        return vector(argv[2]);
    nlp_extra_tests();
    nlp_fabric_tests();
    nlp_profile_tests();
    noise_test();
    session_test();
    transport_test();
    router_input_test();
    assert(nlp_crc32c("123456789", 9) == 0xe3069283);
    puts("NLP_C_CORE_OK");
    return 0;
}
