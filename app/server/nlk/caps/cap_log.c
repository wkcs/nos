/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0002: Kernel Log Stream & Runtime Filter Control.
 */

#include <nos/nlk_caps.h>
#include <kernel/printk.h>
#include <kernel/irq.h>
#include <uapi/nos/errno.h>
#include <string.h>

static void tap_ring_push_bytes(struct nlk_log_ctx *ctx,
                                const uint8_t *src, uint16_t len)
{
    uint16_t i;

    for (i = 0U; i < len; i++) {
        ctx->tap_buf[ctx->tap_head] = src[i];
        ctx->tap_head = (uint16_t)((ctx->tap_head + 1U) % CONFIG_NLK_LOG_TAP_BUF_SIZE);
    }
    ctx->tap_used = (uint16_t)(ctx->tap_used + len);
    if (ctx->tap_used > ctx->high_water) ctx->high_water = ctx->tap_used;
}

static void tap_ring_pop_bytes(struct nlk_log_ctx *ctx,
                               uint8_t *dst, uint16_t len)
{
    uint16_t i;

    for (i = 0U; i < len; i++) {
        dst[i] = ctx->tap_buf[ctx->tap_tail];
        ctx->tap_tail = (uint16_t)((ctx->tap_tail + 1U) % CONFIG_NLK_LOG_TAP_BUF_SIZE);
    }
    ctx->tap_used = (uint16_t)(ctx->tap_used - len);
}

static void tap_ring_make_room(struct nlk_log_ctx *ctx, uint16_t needed)
{
    struct kernel_log_record old_rec;
    uint8_t discard;
    uint16_t rem;

    if (needed > CONFIG_NLK_LOG_TAP_BUF_SIZE) {
        ctx->tap_head = 0U;
        ctx->tap_tail = 0U;
        ctx->tap_used = 0U;
        return;
    }

    while ((uint16_t)(CONFIG_NLK_LOG_TAP_BUF_SIZE - ctx->tap_used) < needed) {
        if (ctx->tap_used < sizeof(old_rec)) {
            ctx->tap_head = 0U;
            ctx->tap_tail = 0U;
            ctx->tap_used = 0U;
            break;
        }
        tap_ring_pop_bytes(ctx, (uint8_t *)&old_rec, sizeof(old_rec));
        if (old_rec.length == 0U || old_rec.length > ctx->tap_used) {
            ctx->tap_head = 0U;
            ctx->tap_tail = 0U;
            ctx->tap_used = 0U;
            break;
        }
        ctx->dropped_records++;
        rem = old_rec.length;
        while (rem-- > 0U) {
            tap_ring_pop_bytes(ctx, &discard, 1U);
        }
    }
}

static int cap_log_on_record(const struct kernel_log_record *record,
                              const void *data, void *priv)
{
    struct nlk_log_ctx *ctx = (struct nlk_log_ctx *)priv;
    uint16_t total_len;

    if (ctx == NULL || record == NULL || data == NULL || record->length == 0U) {
        return 0;
    }
    spin_lock(&ctx->tap_lock);
    if (!ctx->subscribed || !ctx->enabled || record->level < ctx->min_level) {
        spin_unlock(&ctx->tap_lock);
        return 0;
    }
    total_len = (uint16_t)(sizeof(*record) + record->length);
    if (total_len > CONFIG_NLK_LOG_TAP_BUF_SIZE || total_len < record->length) {
        ctx->dropped_records++;
        spin_unlock(&ctx->tap_lock);
        return 0;
    }
    tap_ring_make_room(ctx, total_len);
    tap_ring_push_bytes(ctx, (const uint8_t *)record, sizeof(*record));
    tap_ring_push_bytes(ctx, (const uint8_t *)data, record->length);
    spin_unlock(&ctx->tap_lock);
    return record->length;
}

static unsigned int tap_ring_read_record(struct nlk_log_ctx *ctx,
                                         uint8_t *buf, unsigned int max_len,
                                         struct kernel_log_record *out_rec)
{
    struct kernel_log_record rec;
    addr_t flags;
    uint16_t copy_len;

    flags = disable_irq_save();
    spin_lock(&ctx->tap_lock);
    if (ctx->tap_used < sizeof(rec)) {
        spin_unlock(&ctx->tap_lock);
        enable_irq_save(flags);
        return 0U;
    }
    tap_ring_pop_bytes(ctx, (uint8_t *)&rec, sizeof(rec));
    if (rec.length == 0U || rec.length > ctx->tap_used) {
        ctx->tap_head = 0U;
        ctx->tap_tail = 0U;
        ctx->tap_used = 0U;
        spin_unlock(&ctx->tap_lock);
        enable_irq_save(flags);
        return 0U;
    }
    copy_len = (rec.length <= max_len) ? rec.length : (uint16_t)max_len;
    tap_ring_pop_bytes(ctx, buf, copy_len);
    if (rec.length > copy_len) {
        uint8_t discard;
        uint16_t rem = (uint16_t)(rec.length - copy_len);
        while (rem-- > 0U) {
            tap_ring_pop_bytes(ctx, &discard, 1U);
        }
    }
    spin_unlock(&ctx->tap_lock);
    enable_irq_save(flags);

    rec.length = copy_len;
    if (out_rec != NULL) {
        *out_rec = rec;
    }
    return copy_len;
}

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
        addr_t flags = disable_irq_save();
        spin_lock(&ctx->tap_lock);
        ctx->tap_head = ctx->tap_tail = ctx->tap_used = 0U;
        ctx->pending_len = 0U;
        ctx->tx_fail_count = 0U;
        ctx->subscribed = (sub->enable != 0U);
        ctx->min_level = sub->min_level;
        ctx->flush_interval_ms = sub->flush_interval_ms;
        ctx->subscriber_addr = hdr->src_addr;
        ctx->subscriber_cap = hdr->dst_cap;
        spin_unlock(&ctx->tap_lock);
        enable_irq_save(flags);
        return 0;
    }

    case NLK_LOG_OP_SET_LEVEL: {
        const uint8_t *lvl = (const uint8_t *)req;
        if (req == NULL || req_len < 1U || lvl[0] > LOG_DISABLE) {
            return -EINVAL;
        }
        {
            addr_t flags = disable_irq_save();
            spin_lock(&ctx->tap_lock);
            ctx->min_level = lvl[0];
            if (req_len >= 2U) ctx->enabled = lvl[1] != 0U;
            spin_unlock(&ctx->tap_lock);
            enable_irq_save(flags);
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
        st->dropped_records = ctx->dropped_records;
        st->truncated_records = kstats.truncated_records;
        st->high_water = ctx->high_water;
        st->cur_level = ctx->min_level;
        st->enabled = (ctx->subscribed && ctx->enabled) ? 1U : 0U;
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
    struct nlk_log_wire_hdr wire_hdr;
    struct kernel_log_record rec;
    unsigned int max_msg_len;
    unsigned int read_len;
    int sent = 0;

    if (ctx == NULL || ctx->node == NULL) return 0;
    if (CONFIG_NLK_PACKET_MTU <= sizeof(wire_hdr)) return -ENOSPC;
    if (!ctx->subscribed || !ctx->enabled) return 0;
    max_msg_len = CONFIG_NLK_PACKET_MTU - (unsigned int)sizeof(wire_hdr);

    /* A bounded batch prevents transport diagnostics from feeding an endless
     * flush loop. Keep a failed record staged for the next service iteration. */
    while (sent < 8) {
        int rc;
        if (ctx->pending_len == 0U) {
            read_len = tap_ring_read_record(ctx, ctx->pending_buf + sizeof(wire_hdr),
                                            max_msg_len, &rec);
            if (read_len == 0U) break;
            if (rec.level < ctx->min_level) continue;
            wire_hdr.timestamp_ms = rec.timestamp_ms;
            wire_hdr.length = (uint16_t)read_len;
            wire_hdr.level = rec.level;
            wire_hdr.flags = rec.flags;
            memcpy(ctx->pending_buf, &wire_hdr, sizeof(wire_hdr));
            ctx->pending_len = (uint16_t)(sizeof(wire_hdr) + read_len);
        }
        rc = nlk_node_stream_write(ctx->node, ctx->subscriber_addr,
                                   ctx->cap.cap_id, NLK_LOG_STREAM_RECORD,
                                   ctx->pending_buf, ctx->pending_len);
        if (rc < 0) {
            if (rc != -EAGAIN) {
                ctx->tx_fail_count++;
                /* 当底层链路超时（如 USB 主机端已关闭读取）或连续发送失败时，
                 * 自动解除日志订阅并清空缓冲，避免无限重发与日志自激死循环。 */
                if (rc == -ETIMEDOUT || ctx->tx_fail_count >= 2U) {
                    addr_t flags = disable_irq_save();
                    spin_lock(&ctx->tap_lock);
                    ctx->subscribed = false;
                    ctx->pending_len = 0U;
                    ctx->tap_head = ctx->tap_tail = ctx->tap_used = 0U;
                    ctx->tx_fail_count = 0U;
                    ctx->dropped_records++;
                    spin_unlock(&ctx->tap_lock);
                    enable_irq_save(flags);
                }
            }
            return sent != 0 ? sent : rc;
        }
        ctx->tx_fail_count = 0U;
        ctx->pending_len = 0U;
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
    spin_lock_init(&ctx->tap_lock);
    ctx->min_level = (uint8_t)LOG_INFO;
    ctx->enabled = true;

    ctx->cap.cap_ver = 0x10U;
    ctx->cap.cap_type = NLK_CAP_LOG;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_STREAM;
    ctx->cap.sub_class = 0U;
    ctx->cap.desc_size = 0U;
    ctx->cap.name = "kernel_log";
    ctx->cap.ops = &g_log_ops;
    ctx->cap.priv = ctx;

    {
        int rc = nlk_node_register_cap(node, &ctx->cap);
        if (rc < 0) return rc;
        ctx->backend.write = cap_log_on_record;
        ctx->backend.priv = ctx;
        if (kernel_log_backend_register(&ctx->backend) < 0) return -EINVAL;
        return rc;
    }
}
