/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0002: Kernel Log Stream & Runtime Filter Control.
 */

#include <nos/nlk_caps.h>
#include <kernel/printk.h>
#include <uapi/nos/errno.h>
#include <string.h>

static int cap_log_on_rpc(struct nlk_node *node, struct nlk_capability *cap,
                          const struct nlk_pkt_hdr *hdr,
                          const void *req, uint16_t req_len,
                          void *resp, uint16_t *resp_len)
{
    struct nlk_log_ctx *ctx = (struct nlk_log_ctx *)cap->priv;

    (void)node;
    if (ctx == NULL || resp_len == NULL) {
        return -EINVAL;
    }
    *resp_len = 0U;

    switch (hdr->cmd_op) {
    case NLK_LOG_OP_SUBSCRIBE: {
        const struct nlk_log_sub_req *sub = (const struct nlk_log_sub_req *)req;
        if (req == NULL || req_len < sizeof(*sub)) {
            return -EINVAL;
        }
        ctx->subscribed = (sub->enable != 0U);
        ctx->min_level = sub->min_level;
        ctx->flush_interval_ms = sub->flush_interval_ms;
        ctx->subscriber_addr = hdr->src_addr;
        ctx->subscriber_cap = hdr->dst_cap;
        return 0;
    }

    case NLK_LOG_OP_SET_LEVEL: {
        const uint8_t *lvl = (const uint8_t *)req;
        if (req == NULL || req_len < 1U) {
            return -EINVAL;
        }
        ctx->min_level = lvl[0];
        set_log_level((enum log_level)lvl[0]);
        if (req_len >= 2U) {
            set_log_enabled(lvl[1] != 0U);
        }
        return 0;
    }

    case NLK_LOG_OP_GET_STATS: {
        struct nlk_log_stats_resp *st = (struct nlk_log_stats_resp *)resp;
        struct kernel_log_stats kstats;

        if (resp == NULL) {
            return -EINVAL;
        }
        memset(&kstats, 0, sizeof(kstats));
        kernel_log_get_stats(&kstats);
        st->dropped_records = kstats.dropped_records;
        st->truncated_records = kstats.truncated_records;
        st->high_water = kstats.high_water;
        st->cur_level = ctx->min_level;
        st->enabled = ctx->subscribed ? 1U : 0U;
        st->subscriber_addr = ctx->subscriber_addr;
        *resp_len = sizeof(*st);
        return 0;
    }

    default:
        return -ENOTSUP;
    }
}

int nlk_cap_log_flush_pending(struct nlk_log_ctx *ctx)
{
    uint8_t wire_buf[CONFIG_NLK_PACKET_MTU];
    struct nlk_log_wire_hdr wire_hdr;
    struct kernel_log_record rec;
    unsigned int max_msg_len;
    unsigned int read_len;
    int sent = 0;

    if (ctx == NULL || !ctx->subscribed || ctx->node == NULL) {
        return 0;
    }
    if (CONFIG_NLK_PACKET_MTU <= sizeof(wire_hdr)) {
        return -ENOSPC;
    }
    max_msg_len = CONFIG_NLK_PACKET_MTU - (unsigned int)sizeof(wire_hdr);

    while ((read_len = kernel_log_read_record(wire_buf + sizeof(wire_hdr),
                                              max_msg_len, &rec)) > 0U) {
        if (rec.level < ctx->min_level) {
            continue;
        }
        wire_hdr.timestamp_ms = rec.timestamp_ms;
        wire_hdr.length = (uint16_t)read_len;
        wire_hdr.level = rec.level;
        wire_hdr.flags = rec.flags;
        memcpy(wire_buf, &wire_hdr, sizeof(wire_hdr));

        (void)nlk_node_stream_write(ctx->node, ctx->subscriber_addr,
                                    ctx->cap.cap_id, NLK_LOG_STREAM_RECORD,
                                    wire_buf,
                                    (uint16_t)(sizeof(wire_hdr) + read_len));
        sent++;
    }
    return sent;
}

static const struct nlk_cap_ops g_log_ops = {
    .on_rpc = cap_log_on_rpc,
};

int nlk_cap_log_init(struct nlk_node *node, struct nlk_log_ctx *ctx)
{
    if (node == NULL || ctx == NULL) {
        return -EINVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->node = node;
    ctx->min_level = (uint8_t)LOG_INFO;

    ctx->cap.cap_ver = 0x10U;
    ctx->cap.cap_type = NLK_CAP_LOG;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_STREAM;
    ctx->cap.sub_class = 0U;
    ctx->cap.desc_size = 0U;
    ctx->cap.name = "kernel_log";
    ctx->cap.ops = &g_log_ops;
    ctx->cap.priv = ctx;

    return nlk_node_register_cap(node, &ctx->cap);
}
