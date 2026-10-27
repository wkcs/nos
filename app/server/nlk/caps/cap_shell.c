/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0003: Remote nShell & Virtual TTY Byte-Stream Bridge.
 */

#include <nos/nlk_caps.h>
#if !defined(NLK_HOST_TEST) && __has_include(<kernel/irq.h>)
#include <kernel/types.h>
#include <kernel/irq.h>
#define NLK_SHELL_LOCK()        disable_irq_save()
#define NLK_SHELL_UNLOCK(f)     enable_irq_save(f)
#else
#define NLK_SHELL_LOCK()        (0UL)
#define NLK_SHELL_UNLOCK(f)     ((void)(f))
#endif
#include <uapi/nos/errno.h>
#include <string.h>

static struct nlk_shell_ctx *g_default_shell_ctx;

static int nlk_shell_tty_read(void *context, unsigned char *buf,
                              unsigned int len, bool wait)
{
    struct nlk_shell_ctx *ctx = (struct nlk_shell_ctx *)context;
    unsigned int copied = 0U;
    unsigned long irq_flags;

    (void)wait;
    if (ctx == NULL || buf == NULL || len == 0U) {
        return -EINVAL;
    }
    irq_flags = NLK_SHELL_LOCK();
    if (ctx->rx_count == 0U) {
        NLK_SHELL_UNLOCK(irq_flags);
        return -EAGAIN;
    }
    while (copied < len && ctx->rx_count > 0U) {
        buf[copied++] = ctx->rx_buf[ctx->rx_tail];
        ctx->rx_tail = (uint16_t)((ctx->rx_tail + 1U) % CONFIG_NLK_SHELL_RX_BUF_SIZE);
        ctx->rx_count--;
    }
    NLK_SHELL_UNLOCK(irq_flags);
    return (int)copied;
}

static int nlk_shell_tty_write(void *context, const unsigned char *buf,
                               unsigned int len)
{
    struct nlk_shell_ctx *ctx = (struct nlk_shell_ctx *)context;
    unsigned int sent = 0U;

    if (ctx == NULL || buf == NULL) {
        return -EINVAL;
    }
    if (!ctx->session_open || ctx->node == NULL) {
        return -ENOTCONN;
    }
    while (sent < len) {
        uint16_t chunk = (uint16_t)(len - sent);
        int rc;

        if (chunk > CONFIG_NLK_PACKET_MTU) {
            chunk = CONFIG_NLK_PACKET_MTU;
        }
        rc = nlk_node_stream_write(ctx->node, ctx->client_addr,
                                   ctx->cap.cap_id, NLK_SHELL_STREAM_STDOUT,
                                   buf + sent, chunk);
        if (rc < 0) {
            return (sent > 0U) ? (int)sent : rc;
        }
        sent += chunk;
    }
    return (int)sent;
}

static const struct nos_tty_ops g_nlk_shell_tty_ops = {
    .read  = nlk_shell_tty_read,
    .write = nlk_shell_tty_write,
};

static int cap_shell_on_rpc(struct nlk_node *node, struct nlk_capability *cap,
                            const struct nlk_pkt_hdr *hdr,
                            const void *req, uint16_t req_len,
                            void *resp, uint16_t *resp_len)
{
    struct nlk_shell_ctx *ctx = (struct nlk_shell_ctx *)cap->priv;

    (void)node;
    (void)resp;
    if (ctx == NULL || resp_len == NULL) {
        return -EINVAL;
    }
    *resp_len = 0U;

    switch (hdr->cmd_op) {
    case NLK_SHELL_OP_OPEN: {
        const struct nlk_shell_open_msg *open_msg =
            (const struct nlk_shell_open_msg *)req;
        if (req != NULL && req_len >= sizeof(*open_msg)) {
            ctx->cols = open_msg->cols;
            ctx->rows = open_msg->rows;
            ctx->rx_credit = open_msg->initial_credit;
        } else {
            ctx->cols = 80U;
            ctx->rows = 24U;
            ctx->rx_credit = CONFIG_NLK_SHELL_RX_BUF_SIZE;
        }
        ctx->session_open = true;
        ctx->prompt_pending = true;
        ctx->client_addr = hdr->src_addr;
        ctx->client_cap = hdr->dst_cap;
        return 0;
    }

    case NLK_SHELL_OP_CLOSE:
        ctx->session_open = false;
        ctx->prompt_pending = false;
        return 0;

    case NLK_SHELL_OP_SIGNAL:
        if (req == NULL || req_len < 1U) {
            return -EINVAL;
        }
        ctx->last_signal = *(const uint8_t *)req;
        if (ctx->last_signal == NLK_SHELL_SIG_INT) {
            unsigned long irq_flags = NLK_SHELL_LOCK();
            /* Inject Ctrl+C (0x03) into shell RX stream */
            if (ctx->rx_count < CONFIG_NLK_SHELL_RX_BUF_SIZE) {
                ctx->rx_buf[ctx->rx_head] = 0x03U;
                ctx->rx_head = (uint16_t)((ctx->rx_head + 1U) % CONFIG_NLK_SHELL_RX_BUF_SIZE);
                ctx->rx_count++;
            }
            NLK_SHELL_UNLOCK(irq_flags);
        }
        return 0;

    default:
        return -ENOTSUP;
    }
}

static int cap_shell_on_stream(struct nlk_node *node, struct nlk_capability *cap,
                               const struct nlk_pkt_hdr *hdr,
                               const void *data, uint16_t len)
{
    struct nlk_shell_ctx *ctx = (struct nlk_shell_ctx *)cap->priv;
    const uint8_t *bytes = (const uint8_t *)data;
    uint16_t i;

    (void)node;
    if (ctx == NULL || data == NULL) {
        return -EINVAL;
    }

    if (hdr->cmd_op == NLK_SHELL_OP_FLOW_CRED && len >= sizeof(uint16_t)) {
        uint16_t add_cred;
        memcpy(&add_cred, data, sizeof(add_cred));
        ctx->rx_credit = (uint16_t)(ctx->rx_credit + add_cred);
        return 0;
    }

    if (hdr->cmd_op == NLK_SHELL_STREAM_STDIN ||
        hdr->cmd_op == NLK_SHELL_STREAM_STDOUT) {
        unsigned long irq_flags = NLK_SHELL_LOCK();
        for (i = 0U; i < len; i++) {
            if (ctx->rx_count >= CONFIG_NLK_SHELL_RX_BUF_SIZE) {
                NLK_SHELL_UNLOCK(irq_flags);
                return -ENOSPC;
            }
            ctx->rx_buf[ctx->rx_head] = bytes[i];
            ctx->rx_head = (uint16_t)((ctx->rx_head + 1U) % CONFIG_NLK_SHELL_RX_BUF_SIZE);
            ctx->rx_count++;
        }
        NLK_SHELL_UNLOCK(irq_flags);
        return (int)len;
    }

    return -ENOTSUP;
}

static const struct nlk_cap_ops g_shell_ops = {
    .on_rpc    = cap_shell_on_rpc,
    .on_stream = cap_shell_on_stream,
};

int nlk_cap_shell_init(struct nlk_node *node, struct nlk_shell_ctx *ctx)
{
    int rc;

    if (node == NULL || ctx == NULL) {
        return -EINVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->node = node;
    ctx->cols = 80U;
    ctx->rows = 24U;
    ctx->rx_credit = CONFIG_NLK_SHELL_RX_BUF_SIZE;

    rc = nos_tty_init(&ctx->tty, &g_nlk_shell_tty_ops, ctx);
    if (rc < 0) {
        return rc;
    }

    ctx->cap.cap_ver = 0x10U;
    ctx->cap.cap_type = NLK_CAP_SHELL;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_STREAM;
    ctx->cap.sub_class = 0U;
    ctx->cap.desc_size = 0U;
    ctx->cap.name = "nshell_tty";
    ctx->cap.ops = &g_shell_ops;
    ctx->cap.priv = ctx;

    g_default_shell_ctx = ctx;
    return nlk_node_register_cap(node, &ctx->cap);
}

struct nos_tty *nlk_cap_shell_tty(struct nlk_shell_ctx *ctx)
{
    return (ctx != NULL) ? &ctx->tty : NULL;
}

struct nlk_shell_ctx *nlk_cap_shell_default_ctx(void)
{
    return g_default_shell_ctx;
}
