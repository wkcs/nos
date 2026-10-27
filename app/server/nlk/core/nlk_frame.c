/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) L2 Framing, CRC & Multi-Bearer Codec Implementation.
 */

#include <nos/nlk_core.h>
#include <uapi/nos/errno.h>
#include <string.h>

/* CRC-8 (Polynomial 0x07, Init 0x00) for 2-byte frame length protection */
uint8_t nlk_crc8(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t crc = 0x00U;
    size_t i;
    uint8_t bit;

    if (p == NULL) {
        return 0U;
    }
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (bit = 0; bit < 8U; bit++) {
            if ((crc & 0x80U) != 0U) {
                crc = (uint8_t)((crc << 1U) ^ 0x07U);
            } else {
                crc <<= 1U;
            }
        }
    }
    return crc;
}

/* CRC-16/CCITT-FALSE (Polynomial 0x1021, Init 0xFFFF) */
uint16_t nlk_crc16_ccitt(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFFU;
    size_t i;
    uint8_t bit;

    if (p == NULL) {
        return 0U;
    }
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8U);
        for (bit = 0; bit < 8U; bit++) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            } else {
                crc <<= 1U;
            }
        }
    }
    return crc;
}

/* Standard IEEE 802.3 CRC-32 (incremental) */
uint32_t nlk_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t state = ~crc;
    size_t i;
    uint8_t bit;

    if (p == NULL) {
        return crc;
    }
    for (i = 0; i < len; i++) {
        state ^= p[i];
        for (bit = 0; bit < 8U; bit++) {
            if ((state & 1UL) != 0UL) {
                state = (state >> 1U) ^ 0xEDB88320UL;
            } else {
                state >>= 1U;
            }
        }
    }
    return ~state;
}

void nlk_stream_decoder_init(struct nlk_stream_decoder *dec)
{
    if (dec == NULL) {
        return;
    }
    memset(dec, 0, sizeof(*dec));
    dec->state = NLK_DEC_WAIT_SOF0;
}

/*
 * Encode an L3 NLK packet into an L2 Stream frame:
 * [0x55][0xAA][len_lo][len_hi][hdr_crc8][pkt_buf (pkt_len B)][crc16_lo][crc16_hi]
 */
int nlk_frame_encode_stream(const void *pkt_buf, uint16_t pkt_len,
                            uint8_t *out_frame, uint16_t out_max)
{
    uint8_t len_bytes[2];
    uint16_t tail_crc;

    if (pkt_buf == NULL || out_frame == NULL ||
        pkt_len < sizeof(struct nlk_pkt_hdr) ||
        pkt_len > NLK_MAX_PACKET_SIZE) {
        return -EINVAL;
    }
    if ((uint32_t)pkt_len + NLK_STREAM_OVERHEAD > out_max) {
        return -ENOSPC;
    }

    len_bytes[0] = (uint8_t)(pkt_len & 0xFFU);
    len_bytes[1] = (uint8_t)((pkt_len >> 8U) & 0xFFU);

    out_frame[0] = NLK_SOF_BYTE0;
    out_frame[1] = NLK_SOF_BYTE1;
    out_frame[2] = len_bytes[0];
    out_frame[3] = len_bytes[1];
    out_frame[4] = nlk_crc8(len_bytes, 2U);

    memcpy(&out_frame[5], pkt_buf, pkt_len);
    tail_crc = nlk_crc16_ccitt(pkt_buf, pkt_len);
    out_frame[5U + pkt_len] = (uint8_t)(tail_crc & 0xFFU);
    out_frame[6U + pkt_len] = (uint8_t)((tail_crc >> 8U) & 0xFFU);

    return (int)(pkt_len + NLK_STREAM_OVERHEAD);
}

int nlk_bearer_rx_packet(struct nlk_bearer *bearer,
                         const void *pkt_buf, uint16_t pkt_len)
{
    if (bearer == NULL || bearer->owner_node == NULL ||
        pkt_buf == NULL || pkt_len < sizeof(struct nlk_pkt_hdr)) {
        return -EINVAL;
    }
    return nlk_node_input_packet(bearer->owner_node, bearer, pkt_buf, pkt_len);
}

int nlk_bearer_rx_stream_bytes(struct nlk_bearer *bearer,
                               const uint8_t *bytes, size_t len)
{
    struct nlk_stream_decoder *dec;
    size_t i;
    int dispatched = 0;

    if (bearer == NULL || bytes == NULL) {
        return -EINVAL;
    }
    dec = &bearer->decoder;

    for (i = 0; i < len; i++) {
        uint8_t b = bytes[i];

        switch (dec->state) {
        case NLK_DEC_WAIT_SOF0:
            if (b == NLK_SOF_BYTE0) {
                dec->state = NLK_DEC_WAIT_SOF1;
            }
            break;

        case NLK_DEC_WAIT_SOF1:
            if (b == NLK_SOF_BYTE1) {
                dec->state = NLK_DEC_WAIT_LEN0;
            } else if (b == NLK_SOF_BYTE0) {
                dec->state = NLK_DEC_WAIT_SOF1;
            } else {
                dec->state = NLK_DEC_WAIT_SOF0;
            }
            break;

        case NLK_DEC_WAIT_LEN0:
            dec->expected_len = b;
            dec->state = NLK_DEC_WAIT_LEN1;
            break;

        case NLK_DEC_WAIT_LEN1:
            dec->expected_len |= (uint16_t)((uint16_t)b << 8U);
            dec->state = NLK_DEC_WAIT_HDR_CRC8;
            break;

        case NLK_DEC_WAIT_HDR_CRC8: {
            uint8_t len_bytes[2];
            uint8_t calc_crc8;

            len_bytes[0] = (uint8_t)(dec->expected_len & 0xFFU);
            len_bytes[1] = (uint8_t)((dec->expected_len >> 8U) & 0xFFU);
            calc_crc8 = nlk_crc8(len_bytes, 2U);

            if (b != calc_crc8 ||
                dec->expected_len < sizeof(struct nlk_pkt_hdr) ||
                dec->expected_len > NLK_MAX_PACKET_SIZE) {
                dec->hdr_err_count++;
                dec->state = (b == NLK_SOF_BYTE0) ? NLK_DEC_WAIT_SOF1 : NLK_DEC_WAIT_SOF0;
            } else {
                dec->rx_pos = 0U;
                dec->state = NLK_DEC_WAIT_PAYLOAD;
            }
            break;
        }

        case NLK_DEC_WAIT_PAYLOAD:
            dec->pkt_buf[dec->rx_pos++] = b;
            if (dec->rx_pos >= dec->expected_len) {
                dec->state = NLK_DEC_WAIT_CRC16_0;
            }
            break;

        case NLK_DEC_WAIT_CRC16_0:
            dec->rx_crc16 = b;
            dec->state = NLK_DEC_WAIT_CRC16_1;
            break;

        case NLK_DEC_WAIT_CRC16_1: {
            uint16_t calc_crc16;

            dec->rx_crc16 |= (uint16_t)((uint16_t)b << 8U);
            calc_crc16 = nlk_crc16_ccitt(dec->pkt_buf, dec->expected_len);
            dec->state = NLK_DEC_WAIT_SOF0;

            if (dec->rx_crc16 == calc_crc16) {
                dec->frames_ok++;
                dispatched++;
                (void)nlk_bearer_rx_packet(bearer, dec->pkt_buf, dec->expected_len);
            } else {
                dec->crc_err_count++;
            }
            break;
        }

        default:
            dec->state = NLK_DEC_WAIT_SOF0;
            break;
        }
    }

    return dispatched;
}

static uint8_t nlk_can_prio_from_hdr(const struct nlk_pkt_hdr *hdr)
{
    uint8_t mode = NLK_MSG_MODE(hdr->msg_type);

    switch (mode) {
    case NLK_MODE_EVENT:
        return 0U; /* Highest bus arbitration priority for real-time input events */
    case NLK_MODE_SYS:
        return 1U;
    case NLK_MODE_RPC:
        return 2U;
    case NLK_MODE_STREAM:
        return 5U;
    case NLK_MODE_BULK:
    default:
        return 7U;
    }
}

int nlk_bearer_tx_can_packet(struct nlk_bearer *bearer, uint8_t dst_slot,
                             uint8_t src_slot, const void *pkt_buf,
                             uint16_t pkt_len, uint8_t max_dlc, uint32_t timeout_ms)
{
    const struct nlk_pkt_hdr *hdr = (const struct nlk_pkt_hdr *)pkt_buf;
    const uint8_t *raw = (const uint8_t *)pkt_buf;
    uint8_t prio;
    uint32_t can_id;
    uint16_t offset = 0U;
    uint8_t seq = 0U;
    uint16_t crc16;
    uint8_t frame_buf[64];
    int rc;

    if (bearer == NULL || bearer->ops == NULL || bearer->ops->tx_can_frame == NULL ||
        pkt_buf == NULL || pkt_len < sizeof(struct nlk_pkt_hdr) ||
        max_dlc == 0U || max_dlc > sizeof(frame_buf)) {
        return -EINVAL;
    }

    prio = nlk_can_prio_from_hdr(hdr);

    /* Fit in a single CAN / CAN-FD frame */
    if (pkt_len <= max_dlc) {
        can_id = NLK_CAN_ID_MAKE(prio, dst_slot, src_slot,
                                 hdr->dst_cap, NLK_CAN_SAR_SINGLE, 0U);
        return bearer->ops->tx_can_frame(bearer, can_id, raw, (uint8_t)pkt_len, timeout_ms);
    }

    /* Multi-frame L2-SAR with trailing 2-byte CRC16 */
    crc16 = nlk_crc16_ccitt(pkt_buf, pkt_len);
    while (offset < pkt_len) {
        uint16_t rem = (uint16_t)(pkt_len - offset);
        uint8_t sar;
        uint8_t chunk;

        if (offset == 0U) {
            sar = NLK_CAN_SAR_FIRST;
            chunk = max_dlc;
            memcpy(frame_buf, raw + offset, chunk);
            offset = (uint16_t)(offset + chunk);
        } else if ((uint32_t)rem + 2U <= max_dlc) {
            sar = NLK_CAN_SAR_LAST;
            memcpy(frame_buf, raw + offset, rem);
            frame_buf[rem] = (uint8_t)(crc16 & 0xFFU);
            frame_buf[rem + 1U] = (uint8_t)((crc16 >> 8U) & 0xFFU);
            chunk = (uint8_t)(rem + 2U);
            offset = pkt_len;
        } else {
            sar = NLK_CAN_SAR_CONSEC;
            chunk = (rem > max_dlc) ? max_dlc : (uint8_t)rem;
            memcpy(frame_buf, raw + offset, chunk);
            offset = (uint16_t)(offset + chunk);
        }

        can_id = NLK_CAN_ID_MAKE(prio, dst_slot, src_slot, hdr->dst_cap, sar, seq);
        rc = bearer->ops->tx_can_frame(bearer, can_id, frame_buf, chunk, timeout_ms);
        if (rc < 0) {
            return rc;
        }
        seq = (uint8_t)((seq + 1U) & 0x3FU);

        /* If exact payload ended without room for 2-byte CRC in that frame, send a 2-byte LAST frame */
        if (offset == pkt_len && sar != NLK_CAN_SAR_LAST) {
            frame_buf[0] = (uint8_t)(crc16 & 0xFFU);
            frame_buf[1] = (uint8_t)((crc16 >> 8U) & 0xFFU);
            can_id = NLK_CAN_ID_MAKE(prio, dst_slot, src_slot,
                                     hdr->dst_cap, NLK_CAN_SAR_LAST, seq);
            rc = bearer->ops->tx_can_frame(bearer, can_id, frame_buf, 2U, timeout_ms);
            if (rc < 0) {
                return rc;
            }
        }
    }

    return (int)pkt_len;
}

int nlk_bearer_rx_can_frame(struct nlk_bearer *bearer, uint32_t ext_can_id,
                            const uint8_t *data, uint8_t dlc)
{
    struct nlk_can_reasm *reasm;
    uint8_t sar;
    uint8_t seq;
    uint8_t src_slot;
    uint8_t dst_cap;

    if (bearer == NULL || data == NULL || dlc == 0U) {
        return -EINVAL;
    }

    reasm = &bearer->can_reasm;
    sar = NLK_CAN_ID_SAR(ext_can_id);
    seq = NLK_CAN_ID_SEQ(ext_can_id);
    src_slot = NLK_CAN_ID_SRC_SLOT(ext_can_id);
    dst_cap = NLK_CAN_ID_DST_CAP(ext_can_id);

    if (sar == NLK_CAN_SAR_SINGLE) {
        return nlk_bearer_rx_packet(bearer, data, dlc);
    }

    if (sar == NLK_CAN_SAR_FIRST) {
        if ((size_t)dlc > sizeof(reasm->pkt_buf)) {
            reasm->active = false;
            return -EMSGSIZE;
        }
        reasm->active = true;
        reasm->src_slot = src_slot;
        reasm->dst_cap = dst_cap;
        reasm->next_seq = 1U;
        reasm->rx_len = dlc;
        memcpy(reasm->pkt_buf, data, dlc);
        return 0;
    }

    if (!reasm->active || reasm->src_slot != src_slot ||
        reasm->dst_cap != dst_cap || reasm->next_seq != seq) {
        reasm->active = false;
        return -EPROTO;
    }

    if (sar == NLK_CAN_SAR_CONSEC) {
        if ((uint32_t)reasm->rx_len + dlc > sizeof(reasm->pkt_buf)) {
            reasm->active = false;
            return -ENOSPC;
        }
        memcpy(reasm->pkt_buf + reasm->rx_len, data, dlc);
        reasm->rx_len = (uint16_t)(reasm->rx_len + dlc);
        reasm->next_seq = (uint8_t)((reasm->next_seq + 1U) & 0x3FU);
        return 0;
    }

    if (sar == NLK_CAN_SAR_LAST) {
        uint16_t wire_crc;
        uint16_t calc_crc;
        uint16_t total_pkt_len;

        if ((uint32_t)reasm->rx_len + dlc > sizeof(reasm->pkt_buf) + 2U ||
            (uint32_t)reasm->rx_len + dlc < sizeof(struct nlk_pkt_hdr) + 2U) {
            reasm->active = false;
            return -EPROTO;
        }
        if (dlc > 2U) {
            memcpy(reasm->pkt_buf + reasm->rx_len, data, (size_t)(dlc - 2U));
            reasm->rx_len = (uint16_t)(reasm->rx_len + dlc - 2U);
        }
        wire_crc = (uint16_t)data[dlc - 2U] |
                   (uint16_t)((uint16_t)data[dlc - 1U] << 8U);
        total_pkt_len = reasm->rx_len;
        reasm->active = false;

        calc_crc = nlk_crc16_ccitt(reasm->pkt_buf, total_pkt_len);
        if (wire_crc != calc_crc) {
            return -EBADMSG;
        }
        return nlk_bearer_rx_packet(bearer, reasm->pkt_buf, total_pkt_len);
    }

    return -EINVAL;
}

int nlk_bearer_send_packet(struct nlk_bearer *bearer, uint16_t next_hop_addr,
                           const void *pkt_buf, uint16_t pkt_len, uint32_t timeout_ms)
{
    uint8_t frame_buf[NLK_MAX_FRAME_SIZE];
    int frame_len;

    if (bearer == NULL || bearer->ops == NULL ||
        pkt_buf == NULL || pkt_len < sizeof(struct nlk_pkt_hdr)) {
        return -EINVAL;
    }

    switch (bearer->framing) {
    case NLK_FRAME_STREAM:
        if (bearer->ops->tx_raw == NULL) {
            return -ENOTSUP;
        }
        frame_len = nlk_frame_encode_stream(pkt_buf, pkt_len,
                                            frame_buf, sizeof(frame_buf));
        if (frame_len < 0) {
            return frame_len;
        }
        return bearer->ops->tx_raw(bearer, next_hop_addr,
                                   frame_buf, (uint16_t)frame_len, timeout_ms);

    case NLK_FRAME_DATAGRAM:
    case NLK_FRAME_MASTER_SLAVE:
        if (bearer->ops->tx_raw == NULL) {
            return -ENOTSUP;
        }
        return bearer->ops->tx_raw(bearer, next_hop_addr,
                                   (const uint8_t *)pkt_buf, pkt_len, timeout_ms);

    case NLK_FRAME_CAN: {
        uint8_t max_dlc = (bearer->mtu > 8U) ? 64U : 8U;
        uint8_t dst_slot = (uint8_t)(next_hop_addr & 0x1FU);
        uint8_t src_slot = (bearer->owner_node != NULL) ?
                           (uint8_t)(bearer->owner_node->addr & 0x1FU) : 0U;
        return nlk_bearer_tx_can_packet(bearer, dst_slot, src_slot,
                                        pkt_buf, pkt_len, max_dlc, timeout_ms);
    }

    default:
        return -EINVAL;
    }
}

int nlk_bearer_poll_rx(struct nlk_bearer *bearer, uint32_t timeout_ms)
{
    if (bearer == NULL || bearer->ops == NULL) {
        return -EINVAL;
    }
    if (bearer->ops->rx_poll == NULL) {
        return -ENOTSUP;
    }
    return bearer->ops->rx_poll(bearer, timeout_ms);
}
