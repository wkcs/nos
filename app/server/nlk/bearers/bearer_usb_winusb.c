/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK L1 Bearer Adapter for USB WinUSB (Unified Bulk IN/OUT Datagram).
 */

#define pr_fmt(fmt) "[NLK-USB]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/clk.h>
#include <string.h>
#include "../include/nlk_bearers.h"

#define NLK_WINUSB_TIMEOUT_MS 200U

static struct nlk_winusb_bearer *g_active_winusb;

/* Optional EP0 vendor callback compatibility: feeds control payload into NLK router */
static void nlk_winusb_ep0_callback(uint8_t *buf, size_t size)
{
    if (g_active_winusb != NULL && buf != NULL &&
        size >= sizeof(struct nlk_pkt_hdr) && size <= NLK_MAX_PACKET_SIZE) {
        (void)nlk_bearer_rx_packet(&g_active_winusb->bearer, buf, (uint16_t)size);
    }
}

static int winusb_tx_raw(struct nlk_bearer *b, uint16_t next_hop_addr,
                         const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    struct nlk_winusb_bearer *wb = (struct nlk_winusb_bearer *)b->priv;
    ssize_t written;

    (void)next_hop_addr;
    if (wb == NULL || wb->usb_dev == NULL || buf == NULL) {
        return -EINVAL;
    }
    if (wb->usb_dev->ops.write_timeout != NULL) {
        uint32_t t = (timeout_ms > 0U) ? timeout_ms : wb->timeout_ms;
        written = wb->usb_dev->ops.write_timeout(wb->usb_dev, 0, buf, len,
                                                 msec_to_tick(t));
    } else if (wb->usb_dev->ops.write != NULL) {
        written = wb->usb_dev->ops.write(wb->usb_dev, 0, buf, len);
    } else {
        return -ENOTSUP;
    }
    if (written >= 0 && (size_t)written != len) return -EAGAIN;
    return (int)written;
}

static int winusb_rx_poll(struct nlk_bearer *b, uint32_t timeout_ms);

static const struct nlk_bearer_ops g_winusb_bearer_ops = {
    .tx_raw  = winusb_tx_raw,
    .rx_poll = winusb_rx_poll,
};

int nlk_winusb_bearer_init(struct nlk_node *node,
                           struct nlk_winusb_bearer *wb,
                           uint8_t port_id)
{
    struct device *dev;

    if (node == NULL || wb == NULL) {
        return -EINVAL;
    }
    memset(wb, 0, sizeof(*wb));
    dev = device_find_by_name("winusb");
    if (dev == NULL) {
        return -ENODEV;
    }

    wb->usb_dev = dev;
    wb->timeout_ms = NLK_WINUSB_TIMEOUT_MS;
    wb->bearer.name = "winusb";
    wb->bearer.type = NLK_BEARER_USB_WINUSB;
    wb->bearer.role = (port_id == 0U) ? NLK_ROLE_UPSTREAM_SLAVE : NLK_ROLE_DOWNSTREAM_MASTER;
    wb->bearer.framing = NLK_FRAME_DATAGRAM;
    wb->bearer.port_id = port_id;
    wb->bearer.mtu = CONFIG_NLK_PACKET_MTU;
    wb->bearer.ops = &g_winusb_bearer_ops;
    wb->bearer.priv = wb;

    g_active_winusb = wb;
    if (dev->ops.control != NULL) {
        (void)dev->ops.control(dev, 0x03, nlk_winusb_ep0_callback);
    }

    return nlk_node_register_bearer(node, &wb->bearer);
}

static ssize_t winusb_read_chunk(struct nlk_winusb_bearer *wb,
                                 uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (wb->usb_dev->ops.read_timeout != NULL) {
        uint32_t t = (timeout_ms > 0U) ? timeout_ms : wb->timeout_ms;
        return wb->usb_dev->ops.read_timeout(wb->usb_dev, 0, buf, len,
                                             msec_to_tick(t));
    }
    if (wb->usb_dev->ops.read != NULL) {
        return wb->usb_dev->ops.read(wb->usb_dev, 0, buf, len);
    }
    return -ENOTSUP;
}

int nlk_winusb_bearer_poll_rx(struct nlk_winusb_bearer *wb,
                              uint8_t *rx_buf, uint16_t max_len)
{
    struct nlk_pkt_hdr hdr;
    uint16_t total_len;
    uint16_t got = 0U;
    uint32_t body_timeout_ms;
    ssize_t rc;

    if (wb == NULL || wb->usb_dev == NULL || rx_buf == NULL ||
        max_len < sizeof(struct nlk_pkt_hdr)) {
        return -EINVAL;
    }

    body_timeout_ms = (wb->timeout_ms >= NLK_WINUSB_TIMEOUT_MS) ?
                      wb->timeout_ms : NLK_WINUSB_TIMEOUT_MS;

    while (got < sizeof(struct nlk_pkt_hdr)) {
        uint32_t t_ms = (got == 0U) ? wb->timeout_ms : body_timeout_ms;
        rc = winusb_read_chunk(wb, rx_buf + got,
                               (uint16_t)(sizeof(struct nlk_pkt_hdr) - got),
                               t_ms);
        if (rc <= 0) {
            return (rc < 0) ? (int)rc : -EAGAIN;
        }
        got = (uint16_t)(got + (uint16_t)rc);
    }

    memcpy(&hdr, rx_buf, sizeof(hdr));
    if (NLK_PKT_VER(hdr.ver_flags) != NLK_PROTO_VER_1 ||
        (uint32_t)sizeof(hdr) + hdr.payload_len > max_len) {
        while (winusb_read_chunk(wb, rx_buf, max_len, 2U) > 0) {
        }
        return -EBADMSG;
    }

    total_len = (uint16_t)(sizeof(hdr) + hdr.payload_len);
    while (got < total_len) {
        rc = winusb_read_chunk(wb, rx_buf + got, (uint16_t)(total_len - got),
                               body_timeout_ms);
        if (rc <= 0) {
            return (rc < 0) ? (int)rc : -EAGAIN;
        }
        got = (uint16_t)(got + (uint16_t)rc);
    }

    return nlk_bearer_rx_packet(&wb->bearer, rx_buf, total_len);
}

static int winusb_rx_poll(struct nlk_bearer *b, uint32_t timeout_ms)
{
    struct nlk_winusb_bearer *wb = (struct nlk_winusb_bearer *)b->priv;
    uint32_t saved_timeout;
    int rc;

    if (wb == NULL) {
        return -EINVAL;
    }
    saved_timeout = wb->timeout_ms;
    if (timeout_ms > 0U) {
        wb->timeout_ms = timeout_ms;
    }
    rc = nlk_winusb_bearer_poll_rx(wb, wb->rx_buf, sizeof(wb->rx_buf));
    wb->timeout_ms = saved_timeout;
    return rc;
}
