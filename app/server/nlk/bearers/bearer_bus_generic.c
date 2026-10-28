/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Generic Stream Bearer (UART/SPI/CDC) & Master-Slave Arbiter (I2C/SPI)
 * Implementation. Works in both MCU kernel builds and Host unit tests.
 */

#include "../include/nlk_bearers.h"
#include <uapi/nos/errno.h>
#include <string.h>

#if !defined(NLK_HOST_TEST) && __has_include(<kernel/device.h>)
#include <kernel/device.h>
#define NLK_HAS_KERNEL_DEV 1
#endif

#ifdef NLK_HAS_KERNEL_DEV
static int dev_stream_tx_raw(struct nlk_bearer *b, uint16_t next_hop_addr,
                             const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    struct nlk_dev_stream_bearer *sb = (struct nlk_dev_stream_bearer *)b->priv;

    (void)next_hop_addr;
    (void)timeout_ms;
    if (sb == NULL || sb->dev == NULL || sb->dev->ops.write == NULL) {
        return -EINVAL;
    }
    {
        unsigned int sent = 0U;
        while (sent < len) {
            ssize_t rc = sb->dev->ops.write(sb->dev, 0, buf + sent, len - sent);
            if (rc < 0) return (int)rc;
            if (rc == 0 || (unsigned int)rc > len - sent) return -EIO;
            sent += (unsigned int)rc;
        }
    }
    return (int)len;
}

static int dev_stream_rx_poll(struct nlk_bearer *b, uint32_t timeout_ms)
{
    struct nlk_dev_stream_bearer *sb = (struct nlk_dev_stream_bearer *)b->priv;

    (void)timeout_ms;
    return nlk_dev_stream_bearer_poll_rx(sb);
}

static const struct nlk_bearer_ops g_dev_stream_ops = {
    .tx_raw  = dev_stream_tx_raw,
    .rx_poll = dev_stream_rx_poll,
};

int nlk_dev_stream_bearer_init(struct nlk_node *node,
                               struct nlk_dev_stream_bearer *sb,
                               struct device *dev,
                               enum nlk_bearer_type type,
                               enum nlk_bearer_role role,
                               uint8_t port_id,
                               uint16_t mtu)
{
    if (node == NULL || sb == NULL || dev == NULL) {
        return -EINVAL;
    }
    memset(sb, 0, sizeof(*sb));
    sb->dev = dev;
    sb->timeout_ms = 50U;
    sb->bearer.name = dev->name;
    sb->bearer.type = type;
    sb->bearer.role = role;
    sb->bearer.framing = NLK_FRAME_STREAM;
    sb->bearer.port_id = port_id;
    sb->bearer.mtu = mtu;
    sb->bearer.ops = &g_dev_stream_ops;
    sb->bearer.priv = sb;

    return nlk_node_register_bearer(node, &sb->bearer);
}

int nlk_dev_stream_bearer_poll_rx(struct nlk_dev_stream_bearer *sb)
{
    uint8_t chunk[64];
    ssize_t n;

    if (sb == NULL || sb->dev == NULL || sb->dev->ops.read == NULL) {
        return -EINVAL;
    }
    n = sb->dev->ops.read(sb->dev, 0, chunk, sizeof(chunk));
    if (n <= 0) {
        return -EAGAIN;
    }
    return nlk_bearer_rx_stream_bytes(&sb->bearer, chunk, (size_t)n);
}
#endif /* NLK_HAS_KERNEL_DEV */

/* =========================================================================
 * I2C / Half-Duplex SPI Master-Slave Bus Arbiter
 * ========================================================================= */
static int ms_arbiter_tx_raw(struct nlk_bearer *b, uint16_t next_hop_addr,
                             const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    struct nlk_ms_arbiter *arb = (struct nlk_ms_arbiter *)b->priv;
    uint8_t frame[NLK_MAX_PACKET_SIZE + 4U];
    uint16_t crc16;

    (void)next_hop_addr;
    (void)timeout_ms;
    if (arb == NULL || arb->bus_write == NULL || buf == NULL ||
        len > NLK_MAX_PACKET_SIZE) {
        return -EINVAL;
    }

    frame[0] = (uint8_t)(len & 0xFFU);
    frame[1] = (uint8_t)((len >> 8U) & 0xFFU);
    memcpy(&frame[2], buf, len);
    crc16 = nlk_crc16_ccitt(buf, len);
    frame[2U + len] = (uint8_t)(crc16 & 0xFFU);
    frame[3U + len] = (uint8_t)((crc16 >> 8U) & 0xFFU);

    {
        uint16_t frame_len = (uint16_t)(len + 4U);
        int rc = arb->bus_write(arb->bus_addr, frame, frame_len, arb->bus_priv);
        if (rc < 0) {
            return rc;
        }
        if (rc > 0 && rc != (int)frame_len && rc != (int)len) {
            return -EIO;
        }
        return (int)len;
    }
}

static int ms_arbiter_rx_poll(struct nlk_bearer *b, uint32_t timeout_ms)
{
    struct nlk_ms_arbiter *arb = (struct nlk_ms_arbiter *)b->priv;

    if (arb == NULL) {
        return -EINVAL;
    }
    arb->sim_now_ms += (timeout_ms > 0U) ? timeout_ms : 1U;
    return nlk_ms_arbiter_poll(arb, arb->sim_now_ms);
}

static const struct nlk_bearer_ops g_ms_arbiter_ops = {
    .tx_raw  = ms_arbiter_tx_raw,
    .rx_poll = ms_arbiter_rx_poll,
};

int nlk_ms_master_bearer_init(struct nlk_node *node,
                              struct nlk_ms_arbiter *arb,
                              enum nlk_bearer_type type,
                              uint8_t port_id,
                              uint8_t slave_bus_addr,
                              bool high_freq_input,
                              uint16_t mtu,
                              nlk_ms_bus_write_fn write_fn,
                              nlk_ms_bus_read_fn read_fn,
                              void *bus_priv)
{
    if (node == NULL || arb == NULL || write_fn == NULL || read_fn == NULL) {
        return -EINVAL;
    }
    memset(arb, 0, sizeof(*arb));
    arb->bus_addr = slave_bus_addr;
    arb->bus_write = write_fn;
    arb->bus_read = read_fn;
    arb->bus_priv = bus_priv;

    arb->slot.active = true;
    arb->slot.high_freq_input = high_freq_input;
    arb->slot.bus_addr = slave_bus_addr;
    arb->slot.poll_interval_ms = high_freq_input ? 4U : 200U;

    arb->bearer.name = (type == NLK_BEARER_I2C) ? "i2c-ms" : "spi-ms";
    arb->bearer.type = type;
    arb->bearer.role = NLK_ROLE_DOWNSTREAM_MASTER;
    arb->bearer.framing = NLK_FRAME_MASTER_SLAVE;
    arb->bearer.port_id = port_id;
    arb->bearer.mtu = mtu;
    arb->bearer.ops = &g_ms_arbiter_ops;
    arb->bearer.priv = arb;

    return nlk_node_register_bearer(node, &arb->bearer);
}

void nlk_ms_notify_irq(struct nlk_ms_arbiter *arb)
{
    if (arb != NULL) {
        arb->irq_pending = true;
    }
}

int nlk_ms_arbiter_poll(struct nlk_ms_arbiter *arb, uint32_t now_ms)
{
    struct nlk_ms_poll_hdr status_hdr;
    uint8_t frame_buf[NLK_MAX_PACKET_SIZE + 2U];
    uint16_t wire_crc;
    uint16_t calc_crc;
    int rc;

    if (arb == NULL || !arb->slot.active || arb->bus_read == NULL) {
        return -EINVAL;
    }

    /* Skip poll if neither IRQ triggered nor poll timer elapsed */
    if (!arb->irq_pending &&
        (now_ms - arb->slot.last_poll_ms) < arb->slot.poll_interval_ms) {
        return 0;
    }
    arb->slot.last_poll_ms = now_ms;
    arb->irq_pending = false;

    /* Phase 1: Read 4-byte slave status header */
    rc = arb->bus_read(arb->bus_addr, (uint8_t *)&status_hdr,
                       sizeof(status_hdr), arb->bus_priv);
    if (rc < 0) {
        return rc;
    }
    if (rc > 0 && rc != (int)sizeof(status_hdr)) {
        return -EIO;
    }
    if (status_hdr.magic != NLK_MS_READY_MAGIC || status_hdr.pending_len == 0U) {
        if (arb->slot.consec_empty < 255U) {
            arb->slot.consec_empty++;
        }
        /* Adaptive backoff when idle */
        if (arb->slot.consec_empty > 10U) {
            arb->slot.poll_interval_ms = arb->slot.high_freq_input ? 5U : 250U;
        }
        return 0;
    }

    if (status_hdr.pending_len < sizeof(struct nlk_pkt_hdr) ||
        status_hdr.pending_len > NLK_MAX_PACKET_SIZE) {
        return -EBADMSG;
    }

    /* Phase 2: Read pending NLK packet + 2-byte CRC16 */
    rc = arb->bus_read(arb->bus_addr, frame_buf,
                       (uint16_t)(status_hdr.pending_len + 2U), arb->bus_priv);
    if (rc < 0) {
        return rc;
    }
    if (rc > 0 && rc != (int)(status_hdr.pending_len + 2U)) {
        return -EIO;
    }

    wire_crc = (uint16_t)frame_buf[status_hdr.pending_len] |
               (uint16_t)((uint16_t)frame_buf[status_hdr.pending_len + 1U] << 8U);
    calc_crc = nlk_crc16_ccitt(frame_buf, status_hdr.pending_len);
    if (wire_crc != calc_crc) {
        return -EBADMSG;
    }

    /* Active data received: tighten polling interval */
    arb->slot.consec_empty = 0U;
    arb->slot.poll_interval_ms = arb->slot.high_freq_input ? 1U : 20U;

    return nlk_bearer_rx_packet(&arb->bearer, frame_buf, status_hdr.pending_len);
}

int nlk_ms_slave_prepare_tx(const void *pkt_buf, uint16_t pkt_len,
                            struct nlk_ms_poll_hdr *out_status,
                            uint8_t *out_frame, uint16_t out_max)
{
    uint16_t crc16;

    if (out_status == NULL) {
        return -EINVAL;
    }
    if (pkt_buf == NULL || pkt_len == 0U) {
        out_status->magic = NLK_MS_READY_MAGIC;
        out_status->status_flags = 0U;
        out_status->pending_len = 0U;
        return 0;
    }
    if (out_frame == NULL || (uint32_t)pkt_len + 2U > out_max) {
        return -ENOSPC;
    }
    out_status->magic = NLK_MS_READY_MAGIC;
    out_status->status_flags = 1U;
    out_status->pending_len = pkt_len;

    memcpy(out_frame, pkt_buf, pkt_len);
    crc16 = nlk_crc16_ccitt(pkt_buf, pkt_len);
    out_frame[pkt_len] = (uint8_t)(crc16 & 0xFFU);
    out_frame[pkt_len + 1U] = (uint8_t)((crc16 >> 8U) & 0xFFU);
    return (int)(pkt_len + 2U);
}
