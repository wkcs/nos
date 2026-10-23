/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NLP_WIRE_H
#define NOS_NLP_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include "registry.h"
#ifndef NLP_PAYLOAD_MAX
#define NLP_PAYLOAD_MAX 128u
#endif
#if NLP_PAYLOAD_MAX < 128 || NLP_PAYLOAD_MAX > 1024
#error NLP_PAYLOAD_MAX must be in 128..1024
#endif
#define NLP_HEADER_BYTES 52u
#define NLP_PDU_MAX (NLP_HEADER_BYTES + NLP_PAYLOAD_MAX + 16u)
#define NLP_RELIABLE 1u
#define NLP_ENCRYPTED 2u
#define NLP_CONTROL 0u
#define NLP_REQUEST 1u
#define NLP_RESPONSE 2u
#define NLP_EVENT 3u
#define NLP_DATA 4u
#define NLP_ACK 5u
#define NLP_CLOSE 6u
/* Host representation only. Always serialize through the codec. */
struct nlp_header {
    uint32_t fabric_epoch, session_id, packet_counter, reliable_seq, ack, receive_limit;
    uint16_t src_node, dst_node, src_cap, dst_cap, channel_id, opcode;
    uint8_t kind, flags, priority, hop_limit;
};
struct nlp_frame {
    struct nlp_header h;
    const uint8_t *payload;
    uint16_t length;
};
uint16_t nlp_get16(const uint8_t *);
uint32_t nlp_get32(const uint8_t *);
uint64_t nlp_get64(const uint8_t *);
void nlp_put16(uint8_t *, uint16_t);
void nlp_put32(uint8_t *, uint32_t);
void nlp_put64(uint8_t *, uint64_t);
uint32_t nlp_crc32c(const void *, size_t);
int nlp_header_encode(const struct nlp_header *, size_t, uint8_t out[NLP_HEADER_BYTES]);
/* Header/length checks only; payload must be authenticated before dispatch. */
int nlp_frame_inspect(const uint8_t *, size_t, struct nlp_frame *);
int nlp_frame_encode(const struct nlp_header *, const void *, size_t, uint8_t *, size_t, size_t *);
int nlp_frame_decode(const uint8_t *, size_t, struct nlp_frame *);
void nlp_header_aad(const uint8_t in[NLP_HEADER_BYTES], uint8_t out[NLP_HEADER_BYTES]);
struct nlp_tlv_reader {
    const uint8_t *bytes;
    size_t length, offset;
    uint16_t previous, count;
};
struct nlp_tlv {
    const uint8_t *value;
    uint16_t tag, length;
};
/* next: 1 item, 0 end, negative error; caller rejects unknown critical tags. */
int nlp_tlv_next(struct nlp_tlv_reader *, struct nlp_tlv *);
int nlp_tlv_add(uint8_t *, size_t, size_t *, uint16_t, const void *, size_t);
#endif
