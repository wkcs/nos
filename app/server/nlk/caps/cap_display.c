/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0200: Dual-Mode Display (Semantic Status Sync + Dirty-Rect Stream).
 */

#include <nos/nlk_caps.h>
#include <uapi/nos/errno.h>
#include <string.h>

static int disp_get_desc(struct nlk_node *node, struct nlk_capability *cap,
                         void *buf, uint16_t max_len)
{
    struct nlk_display_ctx *ctx = (struct nlk_display_ctx *)cap->priv;

    (void)node;
    if (buf == NULL || max_len < sizeof(ctx->desc)) {
        return -ENOSPC;
    }
    memcpy(buf, &ctx->desc, sizeof(ctx->desc));
    return (int)sizeof(ctx->desc);
}

static int disp_on_rpc(struct nlk_node *node, struct nlk_capability *cap,
                       const struct nlk_pkt_hdr *hdr,
                       const void *req, uint16_t req_len,
                       void *resp, uint16_t *resp_len)
{
    struct nlk_display_ctx *ctx = (struct nlk_display_ctx *)cap->priv;
    int rc;

    if (ctx == NULL || resp_len == NULL) {
        return -EINVAL;
    }
    *resp_len = 0U;

    switch (hdr->cmd_op) {
    case NLK_DISP_OP_GET_DESC:
        rc = disp_get_desc(node, cap, resp, CONFIG_NLK_PACKET_MTU);
        if (rc < 0) {
            return rc;
        }
        *resp_len = (uint16_t)rc;
        return 0;

    case NLK_DISP_OP_UPDATE_STATUS:
        if (req == NULL || req_len < sizeof(ctx->status)) {
            return -EINVAL;
        }
        memcpy(&ctx->status, req, sizeof(ctx->status));
        return 0;

    case NLK_DISP_OP_SET_BRIGHTNESS:
        if (req == NULL || req_len < 1U) {
            return -EINVAL;
        }
        ctx->desc.brightness = *(const uint8_t *)req;
        return 0;

    case NLK_DISP_OP_DIRTY_RECT: {
        const struct nlk_disp_dirty_rect_hdr *rect =
            (const struct nlk_disp_dirty_rect_hdr *)req;
        if (req == NULL || req_len < sizeof(*rect)) {
            return -EINVAL;
        }
        if (ctx->rect_cb != NULL) {
            return ctx->rect_cb(rect, (const uint8_t *)req + sizeof(*rect),
                                (uint16_t)(req_len - sizeof(*rect)),
                                ctx->rect_priv);
        }
        return 0;
    }

    default:
        return -ENOTSUP;
    }
}

static int disp_on_event(struct nlk_node *node, struct nlk_capability *cap,
                         const struct nlk_pkt_hdr *hdr,
                         const void *evt, uint16_t evt_len)
{
    struct nlk_display_ctx *ctx = (struct nlk_display_ctx *)cap->priv;

    (void)node;
    if (ctx == NULL || evt == NULL) {
        return -EINVAL;
    }
    if (hdr->cmd_op == NLK_DISP_OP_UPDATE_STATUS &&
        evt_len >= sizeof(ctx->status)) {
        memcpy(&ctx->status, evt, sizeof(ctx->status));
        return 0;
    }
    return -ENOTSUP;
}

static int disp_on_stream(struct nlk_node *node, struct nlk_capability *cap,
                          const struct nlk_pkt_hdr *hdr,
                          const void *data, uint16_t len)
{
    struct nlk_display_ctx *ctx = (struct nlk_display_ctx *)cap->priv;
    const struct nlk_disp_dirty_rect_hdr *rect =
        (const struct nlk_disp_dirty_rect_hdr *)data;

    (void)node;
    if (ctx == NULL || data == NULL || hdr->cmd_op != NLK_DISP_OP_DIRTY_RECT ||
        len < sizeof(*rect)) {
        return -EINVAL;
    }
    if (ctx->rect_cb != NULL) {
        return ctx->rect_cb(rect, (const uint8_t *)data + sizeof(*rect),
                            (uint16_t)(len - sizeof(*rect)), ctx->rect_priv);
    }
    return 0;
}

static const struct nlk_cap_ops g_disp_ops = {
    .get_desc  = disp_get_desc,
    .on_rpc    = disp_on_rpc,
    .on_event  = disp_on_event,
    .on_stream = disp_on_stream,
};

int nlk_cap_display_init(struct nlk_node *node, struct nlk_display_ctx *ctx,
                         uint16_t width, uint16_t height, uint8_t pixel_fmt,
                         nlk_disp_rect_cb_t rect_cb, void *priv)
{
    if (node == NULL || ctx == NULL) {
        return -EINVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->node = node;
    ctx->desc.width = width;
    ctx->desc.height = height;
    ctx->desc.pixel_format = pixel_fmt;
    ctx->desc.max_fps = 60U;
    ctx->desc.brightness = 200U;
    ctx->rect_cb = rect_cb;
    ctx->rect_priv = priv;

    ctx->cap.cap_ver = 0x10U;
    ctx->cap.cap_type = NLK_CAP_DISPLAY;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_EVENT | NLK_SUP_MODE_STREAM;
    ctx->cap.sub_class = pixel_fmt;
    ctx->cap.desc_size = sizeof(struct nlk_disp_desc);
    ctx->cap.name = "display";
    ctx->cap.ops = &g_disp_ops;
    ctx->cap.priv = ctx;

    return nlk_node_register_cap(node, &ctx->cap);
}

int nlk_cap_display_push_status(struct nlk_node *node, nlk_addr_t dst_addr,
                                uint8_t dst_cap,
                                const struct nlk_disp_status_sync *status)
{
    if (node == NULL || status == NULL) {
        return -EINVAL;
    }
    return nlk_node_send_event(node, dst_addr, dst_cap,
                               NLK_DISP_OP_UPDATE_STATUS,
                               status, sizeof(*status), false);
}
