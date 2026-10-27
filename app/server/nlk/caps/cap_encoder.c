/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0101: Rotary Knob / Haptic Encoder.
 */

#include <nos/nlk_caps.h>
#include <uapi/nos/errno.h>
#include <string.h>

static int enc_get_desc(struct nlk_node *node, struct nlk_capability *cap,
                        void *buf, uint16_t max_len)
{
    struct nlk_encoder_ctx *ctx = (struct nlk_encoder_ctx *)cap->priv;

    (void)node;
    if (buf == NULL || max_len < sizeof(ctx->desc)) {
        return -ENOSPC;
    }
    memcpy(buf, &ctx->desc, sizeof(ctx->desc));
    return (int)sizeof(ctx->desc);
}

static int enc_on_rpc(struct nlk_node *node, struct nlk_capability *cap,
                      const struct nlk_pkt_hdr *hdr,
                      const void *req, uint16_t req_len,
                      void *resp, uint16_t *resp_len)
{
    struct nlk_encoder_ctx *ctx = (struct nlk_encoder_ctx *)cap->priv;
    int rc;

    if (ctx == NULL || resp_len == NULL) {
        return -EINVAL;
    }
    *resp_len = 0U;

    switch (hdr->cmd_op) {
    case NLK_ENC_OP_GET_DESC:
        rc = enc_get_desc(node, cap, resp, CONFIG_NLK_PACKET_MTU);
        if (rc < 0) {
            return rc;
        }
        *resp_len = (uint16_t)rc;
        return 0;

    case NLK_ENC_OP_SET_CONFIG: {
        const struct nlk_encoder_cfg *cfg = (const struct nlk_encoder_cfg *)req;
        if (req == NULL || req_len < sizeof(*cfg) || cfg->layer >= NLK_KB_MAX_LAYERS) {
            return -EINVAL;
        }
        ctx->cfg[cfg->layer] = *cfg;
        return 0;
    }

    default:
        return -ENOTSUP;
    }
}

static int enc_on_event(struct nlk_node *node, struct nlk_capability *cap,
                        const struct nlk_pkt_hdr *hdr,
                        const void *evt, uint16_t evt_len)
{
    struct nlk_encoder_ctx *ctx = (struct nlk_encoder_ctx *)cap->priv;

    (void)node;
    if (ctx == NULL || evt == NULL || hdr->cmd_op != NLK_ENC_EVT_CHANGE ||
        evt_len < sizeof(ctx->last_event)) {
        return -EINVAL;
    }
    memcpy(&ctx->last_event, evt, sizeof(ctx->last_event));
    if (ctx->evt_cb != NULL) {
        ctx->evt_cb(&ctx->last_event, ctx->evt_priv);
    }
    return 0;
}

static const struct nlk_cap_ops g_enc_ops = {
    .get_desc = enc_get_desc,
    .on_rpc   = enc_on_rpc,
    .on_event = enc_on_event,
};

int nlk_cap_encoder_init(struct nlk_node *node, struct nlk_encoder_ctx *ctx,
                         uint8_t steps_per_rev, bool has_haptic,
                         nlk_enc_event_cb_t evt_cb, void *priv)
{
    if (node == NULL || ctx == NULL) {
        return -EINVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->node = node;
    ctx->desc.encoder_count = 1U;
    ctx->desc.has_switch = 1U;
    ctx->desc.has_haptic = has_haptic ? 1U : 0U;
    ctx->desc.steps_per_rev = steps_per_rev;
    ctx->evt_cb = evt_cb;
    ctx->evt_priv = priv;

    ctx->cap.cap_ver = 0x10U;
    ctx->cap.cap_type = NLK_CAP_ENCODER;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_EVENT;
    ctx->cap.sub_class = 0U;
    ctx->cap.desc_size = sizeof(struct nlk_encoder_desc);
    ctx->cap.name = "encoder";
    ctx->cap.ops = &g_enc_ops;
    ctx->cap.priv = ctx;

    return nlk_node_register_cap(node, &ctx->cap);
}

int nlk_cap_encoder_send_delta(struct nlk_encoder_ctx *ctx,
                               nlk_addr_t dst_addr, uint8_t dst_cap,
                               uint8_t encoder_id, int16_t delta_steps,
                               uint8_t button_state, uint16_t abs_angle_deg10)
{
    struct nlk_encoder_event evt;

    if (ctx == NULL || ctx->node == NULL) {
        return -EINVAL;
    }
    evt.encoder_id = encoder_id;
    evt.button_state = button_state;
    evt.delta_steps = delta_steps;
    evt.abs_angle_deg10 = abs_angle_deg10;

    return nlk_node_send_event(ctx->node, dst_addr, dst_cap,
                               NLK_ENC_EVT_CHANGE, &evt, sizeof(evt), false);
}
