/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0300: Bluetooth / 2.4G / Wi-Fi Co-processor &
 * Dynamic Upstream Host Gateway Switching.
 */

#include <nos/nlk_caps.h>
#include <uapi/nos/errno.h>
#include <string.h>

static int wl_get_desc(struct nlk_node *node, struct nlk_capability *cap,
                       void *buf, uint16_t max_len)
{
    struct nlk_wireless_ctx *ctx = (struct nlk_wireless_ctx *)cap->priv;

    (void)node;
    if (buf == NULL || max_len < sizeof(ctx->info)) {
        return -ENOSPC;
    }
    memcpy(buf, &ctx->info, sizeof(ctx->info));
    return (int)sizeof(ctx->info);
}

static int wl_on_rpc(struct nlk_node *node, struct nlk_capability *cap,
                     const struct nlk_pkt_hdr *hdr,
                     const void *req, uint16_t req_len,
                     void *resp, uint16_t *resp_len)
{
    struct nlk_wireless_ctx *ctx = (struct nlk_wireless_ctx *)cap->priv;
    int rc;

    if (ctx == NULL || resp_len == NULL) {
        return -EINVAL;
    }
    *resp_len = 0U;

    switch (hdr->cmd_op) {
    case NLK_WL_OP_GET_INFO:
        rc = wl_get_desc(node, cap, resp, CONFIG_NLK_PACKET_MTU);
        if (rc < 0) {
            return rc;
        }
        *resp_len = (uint16_t)rc;
        return 0;

    case NLK_WL_OP_SWITCH_CHANNEL: {
        const struct nlk_wl_switch_req *sw = (const struct nlk_wl_switch_req *)req;
        if (req == NULL || req_len < sizeof(*sw)) {
            return -EINVAL;
        }
        if (((1U << sw->target_channel) & ctx->info.sup_channels_mask) == 0U) {
            return -ENOTSUP;
        }
        ctx->info.active_channel = sw->target_channel;
        ctx->info.conn_state = (sw->start_pairing != 0U) ?
                               NLK_WL_STATE_PAIRING : NLK_WL_STATE_CONNECTED;
        return 0;
    }

    default:
        return -ENOTSUP;
    }
}

static int wl_on_event(struct nlk_node *node, struct nlk_capability *cap,
                       const struct nlk_pkt_hdr *hdr,
                       const void *evt, uint16_t evt_len)
{
    struct nlk_wireless_ctx *ctx = (struct nlk_wireless_ctx *)cap->priv;
    const struct nlk_wl_state_evt *sevt = (const struct nlk_wl_state_evt *)evt;

    (void)node;
    if (ctx == NULL || evt == NULL || hdr->cmd_op != NLK_WL_EVT_LINK_STATE ||
        evt_len < sizeof(*sevt)) {
        return -EINVAL;
    }
    ctx->info.active_channel = sevt->active_channel;
    ctx->info.conn_state = sevt->conn_state;
    ctx->info.rssi_dbm = sevt->rssi_dbm;
    ctx->info.host_led_locks = sevt->host_led_locks;
    return 0;
}

static const struct nlk_cap_ops g_wl_ops = {
    .get_desc = wl_get_desc,
    .on_rpc   = wl_on_rpc,
    .on_event = wl_on_event,
};

int nlk_cap_wireless_init(struct nlk_node *node, struct nlk_wireless_ctx *ctx,
                          uint8_t sup_channels_mask, uint8_t bridge_port_id)
{
    if (node == NULL || ctx == NULL) {
        return -EINVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->node = node;
    ctx->bridge_port_id = bridge_port_id;
    ctx->info.sup_channels_mask = sup_channels_mask;
    ctx->info.active_channel = NLK_WL_CHAN_BLE_SLOT1;
    ctx->info.conn_state = NLK_WL_STATE_CONNECTED;
    ctx->info.rssi_dbm = -48;
    ctx->info.battery_pct = 95U;

    ctx->cap.cap_ver = 0x10U;
    ctx->cap.cap_type = NLK_CAP_WIRELESS;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_EVENT;
    ctx->cap.sub_class = 0U;
    ctx->cap.desc_size = sizeof(struct nlk_wl_info_resp);
    ctx->cap.name = "wireless";
    ctx->cap.ops = &g_wl_ops;
    ctx->cap.priv = ctx;

    return nlk_node_register_cap(node, &ctx->cap);
}

int nlk_cap_wireless_switch_host_channel(struct nlk_node *master_node,
                                         nlk_addr_t wl_node_addr, uint8_t wl_cap_id,
                                         uint8_t wl_port_id, uint8_t target_channel)
{
    struct nlk_wl_switch_req req;
    uint16_t resp_len = 0U;
    int rc;

    if (master_node == NULL) {
        return -EINVAL;
    }

    if (target_channel == NLK_WL_CHAN_USB_WIRED) {
        /* Switch upstream gateway back to wired USB (port 0) */
        return nlk_node_set_upstream_port(master_node, 0U);
    }

    memset(&req, 0, sizeof(req));
    req.target_channel = target_channel;
    req.start_pairing = 0U;
    req.clear_bonds = 0U;
    req.tx_power_level = 4U;

    rc = nlk_node_rpc_call(master_node, wl_node_addr, wl_cap_id,
                           NLK_WL_OP_SWITCH_CHANNEL, &req, sizeof(req),
                           NULL, &resp_len, 200U);
    if (rc < 0) {
        return rc;
    }

    /* Rebind upstream Host (0x0000) route to the wireless module's port */
    return nlk_node_set_upstream_port(master_node, wl_port_id);
}
