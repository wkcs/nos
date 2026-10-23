/* Copyright (C) 2026 NOS contributors. RPC execution and hostile input regressions. */
#include "nlp/endpoint.h"
#include "nlp/transport.h"
#include "nlp/text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned executions, total;
static int sink(void *context, const uint8_t *bytes, size_t size)
{
    (void)context;
    (void)bytes;
    (void)size;
    return 0;
}
static int mutate(void *context, struct nlp_binding *b, unsigned op, const uint8_t *in, size_t size,
                  uint8_t *out, size_t *used)
{
    (void)context;
    (void)b;
    assert(op == 1 && size == 4 && *used >= 4);
    ++executions;
    total += nlp_get32(in);
    nlp_put32(out, total);
    *used = 4;
    return 0;
}
static void acknowledge(struct nlp_session *s, uint32_t ack)
{
    struct nlp_header h = {.kind = NLP_ACK,
                           .priority = 0,
                           .hop_limit = 16,
                           .fabric_epoch = 1,
                           .src_node = 2,
                           .dst_node = 1,
                           .src_cap = 0,
                           .dst_cap = s->local_cap,
                           .channel_id = 1,
                           .session_id = s->id,
                           .packet_counter = ack,
                           .ack = ack,
                           .receive_limit = ack + 1};
    uint8_t raw[NLP_PDU_MAX];
    size_t size;
    assert(!nlp_frame_encode(&h, NULL, 0, raw, sizeof(raw), &size));
    assert(!nlp_session_receive(s, raw, size, ack));
}
static struct nlp_frame queued(struct nlp_endpoint *e, struct nlp_session *s)
{
    struct nlp_frame f;
    int found = 0;
    for (unsigned i = 0; i < NLP_POOL_SLOTS; ++i)
        if (e->pool.slot[i].owner == s && e->pool.slot[i].role == 1) {
            assert(!nlp_frame_decode(e->pool.slot[i].bytes, e->pool.slot[i].length, &f));
            found = 1;
            break;
        }
    assert(found);
    return f;
}
static void rpc_test(void)
{
    struct nlp_endpoint e;
    struct nlp_endpoint_config c = {.uid = {3},
                                    .fabric = {5},
                                    .boot_id = 1,
                                    .epoch = 1,
                                    .permissions = 1,
                                    .node = 1,
                                    .physical = 1,
                                    .send = sink};
    struct nlp_capability cap = {{7}, 1, 1, 0, 1, 0, 1, mutate, NULL, NULL};
    assert(!nlp_endpoint_init(&e, &c) && !nlp_endpoint_add(&e, &cap));
    e.seen = 1;
    e.peer_payload = 128;
    e.peer_uid[0] = 1;
    e.peer_boot = 1;
    assert(!nlp_endpoint_associate(&e, 2, 1, NULL, NULL));
    struct nlp_binding *manager = &e.bindings[0];
    manager->session.channel[1].tx_limit = 1;
    uint8_t request[60] = {0};
    nlp_put32(request, 1);
    nlp_put32(request + 8, 2);
    nlp_put16(request + 12, 1);
    memcpy(request + 16, cap.type, 16);
    nlp_put16(request + 32, 1);
    nlp_put16(request + 48, 128);
    request[50] = 1;
    request[51] = 2;
    request[52] = 1;
    nlp_put32(request + 56, 15000);
    struct nlp_frame f = {.h = {.kind = NLP_REQUEST, .channel_id = 1, .opcode = NLP_OP_OPEN},
                          .payload = request,
                          .length = 60};
    assert(!manager->session.message(manager, &manager->session, &f));
    acknowledge(&manager->session, 1);
    struct nlp_binding *b = &e.bindings[1];
    assert(b->session.active && b->session.id == 2);
    b->session.channel[1].tx_limit = 1;
    uint8_t body[12] = {0};
    nlp_put32(body, 1);
    nlp_put32(body + 8, 7);
    f.h.opcode = 1;
    f.payload = body;
    f.length = 12;
    assert(!b->session.message(b, &b->session, &f) && executions == 1 && total == 7);
    assert(b->session.message(b, &b->session, &f) == -NLP_BUSY && executions == 1);
    acknowledge(&b->session, 1);
    assert(!b->session.message(b, &b->session, &f) && executions == 1);
    struct nlp_frame response = queued(&e, &b->session);
    assert(nlp_get32(response.payload) == 1 && !nlp_get16(response.payload + 4) &&
           nlp_get32(response.payload + 8) == 7);
    body[8] = 8;
    assert(b->session.message(b, &b->session, &f) == -NLP_CONFLICT && executions == 1);
    nlp_put32(body, 2);
    nlp_put32(body + 8, 11);
    assert(b->session.message(b, &b->session, &f) == -NLP_BUSY && executions == 2 && total == 18);
    acknowledge(&b->session, 2);
    assert(!b->session.message(b, &b->session, &f) && executions == 2);
    acknowledge(&b->session, 3);
    nlp_put32(body, 1);
    assert(!b->session.message(b, &b->session, &f) && executions == 2);
    response = queued(&e, &b->session);
    assert(nlp_get16(response.payload + 4) == NLP_OUTCOME_UNKNOWN && response.length == 8);
    puts("PASS nlp_rpc_side_effect_once_under_response_backpressure_conflict_and_evicted_result");
}
static uint32_t random_state = 0x4e4c5046;
static uint32_t random_word(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}
static int drop(void *context, const uint8_t *bytes, size_t size)
{
    struct nlp_frame f;
    (void)context;
    return nlp_frame_decode(bytes, size, &f);
}
static void fuzz(void)
{
    uint8_t raw[NLP_PDU_MAX], input[256];
    struct nlp_stream stream;
    struct nlp_can can = {0};
    nlp_stream_init(&stream, NLP_FRAMING_UART);
    nlp_can_reset(&can, 2);
    for (unsigned iteration = 0; iteration < 30000; ++iteration) {
        size_t size = random_word() % sizeof(input), consumed, prefix;
        for (size_t i = 0; i < size; ++i)
            input[i] = (uint8_t)(random_word() >> 24);
        struct nlp_frame f;
        int error = nlp_frame_decode(input, size, &f);
        assert(!error || (error < 0 && error >= -NLP_MTU_TOO_SMALL));
        (void)nlp_utf8_prefix(input, size, size, &prefix);
        assert(!nlp_stream_feed(&stream, input, size, &consumed, iteration, drop, NULL) &&
               consumed == size);
        assert(stream.used < sizeof(stream.bytes));
        struct nlp_tlv_reader reader = {input, size, 0, 0, 0};
        struct nlp_tlv t;
        while (nlp_tlv_next(&reader, &t) > 0)
            assert(reader.offset <= size && reader.count <= 32);
        size_t can_size = size % 65;
        if (can_size > size)
            can_size = size;
        (void)nlp_can_receive(&can, 0x04040800u | (random_word() % 64) << 4, input, can_size,
                              iteration, drop, NULL);
        struct nlp_header h = {.kind = 3,
                               .priority = 1,
                               .hop_limit = 16,
                               .fabric_epoch = 1,
                               .src_node = 1,
                               .dst_node = 2,
                               .session_id = 1,
                               .packet_counter = iteration + 1};
        size_t written;
        assert(!nlp_frame_encode(&h, NULL, 0, raw, sizeof(raw), &written));
        unsigned offset = random_word() % 52;
        raw[offset] ^= (uint8_t)(random_word() | 1);
        nlp_put32(raw + 48, nlp_crc32c(raw, 48));
        nlp_put32(raw + written - 4, nlp_crc32c(raw, written - 4));
        (void)nlp_frame_decode(raw, written, &f);
    }
    puts("PASS nlp_30000_hostile_wire_tlv_utf8_uart_can_inputs_with_bounded_reassembly");
}
void nlp_extra_tests(void)
{
    rpc_test();
    fuzz();
}
