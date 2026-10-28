/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0003: Remote nShell & Virtual TTY Byte-Stream Bridge.
 */

#include <nos/nlk_caps.h>
#if !defined(NLK_HOST_TEST) && __has_include(<kernel/irq.h>)
#include <kernel/types.h>
#include <kernel/irq.h>
#define NLK_SHELL_LOCK(ctx)     nlk_shell_lock(ctx)
#define NLK_SHELL_UNLOCK(ctx, f) nlk_shell_unlock(ctx, f)
#else
#define NLK_SHELL_LOCK(ctx)      (0UL)
#define NLK_SHELL_UNLOCK(ctx, f) ((void)(f))
#endif
#include <uapi/nos/errno.h>
#include <string.h>

static struct nlk_shell_ctx *g_default_shell_ctx;

#if !defined(NLK_HOST_TEST) && __has_include(<kernel/irq.h>)
static unsigned long nlk_shell_lock(struct nlk_shell_ctx *ctx)
{
    unsigned long flags = disable_irq_save();
    spin_lock(&ctx->io_lock);
    return flags;
}

static void nlk_shell_unlock(struct nlk_shell_ctx *ctx, unsigned long flags)
{
    spin_unlock(&ctx->io_lock);
    enable_irq_save(flags);
}
#endif


static int shell_read(struct nlk_shell_ctx *ctx, uint32_t generation, bool check_generation,
                      unsigned char *buf, unsigned int len)
{
    unsigned int copied = 0U;
    unsigned long irq_flags;

    if (ctx == NULL || buf == NULL || len == 0U) return -EINVAL;
    irq_flags = NLK_SHELL_LOCK(ctx);
    if (!ctx->session_open || (check_generation && ctx->generation != generation)) {
        NLK_SHELL_UNLOCK(ctx, irq_flags);
        return -ENOTCONN;
    }
    while (copied < len && ctx->rx_count > 0U) {
        buf[copied++] = ctx->rx_buf[ctx->rx_tail];
        ctx->rx_tail = (uint16_t)((ctx->rx_tail + 1U) % CONFIG_NLK_SHELL_RX_BUF_SIZE);
        ctx->rx_count--;
    }
    NLK_SHELL_UNLOCK(ctx, irq_flags);
    return copied != 0U ? (int)copied : -EAGAIN;
}

int nlk_cap_shell_read_session(struct nlk_shell_ctx *ctx, uint32_t generation,
                               unsigned char *buf, unsigned int len)
{
    return shell_read(ctx, generation, true, buf, len);
}

static int nlk_shell_tty_read(void *context, unsigned char *buf,
                              unsigned int len, bool wait)
{
    (void)wait;
    return shell_read(context, 0, false, buf, len);
}

static int shell_write(struct nlk_shell_ctx *ctx, uint32_t generation, bool check_generation,
                       const unsigned char *buf, unsigned int len)
{
    unsigned int copied = 0U;
    unsigned long irq_flags;

    if (ctx == NULL || buf == NULL) return -EINVAL;
    irq_flags = NLK_SHELL_LOCK(ctx);
    if (!ctx->session_open || ctx->node == NULL ||
        (check_generation && ctx->generation != generation)) {
        NLK_SHELL_UNLOCK(ctx, irq_flags);
        return -ENOTCONN;
    }
    while (copied < len && ctx->tx_count < CONFIG_NLK_SHELL_TX_BUF_SIZE) {
        ctx->tx_buf[ctx->tx_head] = buf[copied++];
        ctx->tx_head = (uint16_t)((ctx->tx_head + 1U) % CONFIG_NLK_SHELL_TX_BUF_SIZE);
        ctx->tx_count++;
    }
    NLK_SHELL_UNLOCK(ctx, irq_flags);
    return copied != 0U ? (int)copied : -EAGAIN;
}

int nlk_cap_shell_write_session(struct nlk_shell_ctx *ctx, uint32_t generation,
                                const unsigned char *buf, unsigned int len)
{
    return shell_write(ctx, generation, true, buf, len);
}

static int nlk_shell_tty_write(void *context, const unsigned char *buf,
                               unsigned int len)
{
    return shell_write(context, 0, false, buf, len);
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

    if (hdr == NULL) return -EINVAL;
    switch (hdr->cmd_op) {
    case NLK_SHELL_OP_OPEN: {
        const struct nlk_shell_open_msg *open_msg =
            (const struct nlk_shell_open_msg *)req;
        uint64_t session_id = 0U;
        bool reliable = req != NULL && req_len >= sizeof(*open_msg) &&
                        (open_msg->flags & NLK_SHELL_OPEN_RELIABLE) != 0U;
        unsigned long irq_flags;
        if (reliable) {
            if (req_len < sizeof(struct nlk_shell_open_v2_msg)) return -EINVAL;
            memcpy(&session_id, (const uint8_t *)req + sizeof(*open_msg), sizeof(session_id));
            if (session_id == 0U) return -EINVAL;
        }
        irq_flags = NLK_SHELL_LOCK(ctx);
        if (ctx->session_open) {
            bool retry = reliable && ctx->reliable && ctx->session_id == session_id &&
                         ctx->client_addr == hdr->src_addr;
            if (retry) ctx->lease_touched = true;
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return retry ? 0 : -EBUSY;
        }
        ctx->reliable = reliable;
        ctx->session_id = session_id;
        ctx->input_offset = 0U;
        ctx->lease_started = false;
        ctx->lease_touched = true;
        ctx->rx_head = ctx->rx_tail = ctx->rx_count = 0U;
        ctx->tx_head = ctx->tx_tail = ctx->tx_count = 0U;
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
        __atomic_add_fetch(&ctx->generation, 1U, __ATOMIC_RELEASE);
        NLK_SHELL_UNLOCK(ctx, irq_flags);
        return 0;
    }

    case NLK_SHELL_OP_CLOSE:
        {
            unsigned long irq_flags = NLK_SHELL_LOCK(ctx);
            if (!ctx->session_open || ctx->client_addr != hdr->src_addr) {
                int rc = ctx->session_open ? -EPERM : -ENOTCONN;
                NLK_SHELL_UNLOCK(ctx, irq_flags);
                return rc;
            }
            if (ctx->reliable) {
                uint64_t token;
                if (req == NULL || req_len < sizeof(token)) {
                    NLK_SHELL_UNLOCK(ctx, irq_flags);
                    return -EPERM;
                }
                memcpy(&token, req, sizeof(token));
                if (token != ctx->session_id) {
                    NLK_SHELL_UNLOCK(ctx, irq_flags);
                    return -EPERM;
                }
            }
            ctx->session_open = false;
            ctx->prompt_pending = false;
            ctx->rx_head = ctx->rx_tail = ctx->rx_count = 0U;
            ctx->tx_head = ctx->tx_tail = ctx->tx_count = 0U;
            __atomic_add_fetch(&ctx->generation, 1U, __ATOMIC_RELEASE);
            NLK_SHELL_UNLOCK(ctx, irq_flags);
        }
        return 0;

    case NLK_SHELL_OP_INPUT:
    case NLK_SHELL_OP_HEARTBEAT: {
        struct nlk_shell_input_hdr input;
        unsigned int n, i;
        unsigned long irq_flags;
        uint64_t token;
        if (req == NULL || req_len < sizeof(token)) return -EINVAL;
        memcpy(&token, req, sizeof(token));
        irq_flags = NLK_SHELL_LOCK(ctx);
        if (!ctx->session_open || !ctx->reliable || ctx->client_addr != hdr->src_addr ||
            ctx->session_id != token) {
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return -EPERM;
        }
        ctx->lease_touched = true;
        if (hdr->cmd_op == NLK_SHELL_OP_HEARTBEAT) {
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return 0;
        }
        if (req_len <= sizeof(input)) {
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return -EINVAL;
        }
        memcpy(&input, req, sizeof(input));
        n = req_len - sizeof(input);
        if (n > NLK_SHELL_INPUT_CHUNK_MAX || n > CONFIG_NLK_SHELL_RX_BUF_SIZE) {
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return -EMSGSIZE;
        }
        if (input.offset != ctx->input_offset) {
            bool duplicate = (uint32_t)(ctx->input_offset - input.offset) == n;
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return duplicate ? 0 : -EINVAL;
        }
        if (n > CONFIG_NLK_SHELL_RX_BUF_SIZE - ctx->rx_count) {
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return -ENOSPC;
        }
        for (i = 0U; i < n; i++) {
            ctx->rx_buf[ctx->rx_head] = ((const uint8_t *)req)[sizeof(input) + i];
            ctx->rx_head = (uint16_t)((ctx->rx_head + 1U) % CONFIG_NLK_SHELL_RX_BUF_SIZE);
            ctx->rx_count++;
        }
        ctx->input_offset += n;
        NLK_SHELL_UNLOCK(ctx, irq_flags);
        return 0;
    }

    case NLK_SHELL_OP_SIGNAL:
        if (req == NULL || req_len < 1U) {
            return -EINVAL;
        }
        {
            unsigned long irq_flags = NLK_SHELL_LOCK(ctx);
            if (!ctx->session_open || ctx->client_addr != hdr->src_addr) {
                int rc = ctx->session_open ? -EPERM : -ENOTCONN;
                NLK_SHELL_UNLOCK(ctx, irq_flags);
                return rc;
            }
            if (ctx->reliable) {
                uint64_t token;
                if (req_len < sizeof(token) + 1U) {
                    NLK_SHELL_UNLOCK(ctx, irq_flags);
                    return -EPERM;
                }
                memcpy(&token, req, sizeof(token));
                if (token != ctx->session_id) {
                    NLK_SHELL_UNLOCK(ctx, irq_flags);
                    return -EPERM;
                }
                ctx->last_signal = ((const uint8_t *)req)[sizeof(token)];
                ctx->lease_touched = true;
            } else {
                ctx->last_signal = *(const uint8_t *)req;
                ctx->lease_touched = true;
            }
            if (ctx->last_signal == NLK_SHELL_SIG_INT) {
                if (ctx->rx_count >= CONFIG_NLK_SHELL_RX_BUF_SIZE) {
                    NLK_SHELL_UNLOCK(ctx, irq_flags);
                    return -ENOSPC;
                }
                ctx->rx_buf[ctx->rx_head] = 0x03U;
                ctx->rx_head = (uint16_t)((ctx->rx_head + 1U) % CONFIG_NLK_SHELL_RX_BUF_SIZE);
                ctx->rx_count++;
            }
            NLK_SHELL_UNLOCK(ctx, irq_flags);
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
    if (ctx == NULL || hdr == NULL || data == NULL) {
        return -EINVAL;
    }
    {
        unsigned long irq_flags = NLK_SHELL_LOCK(ctx);
        if (!ctx->session_open || ctx->client_addr != hdr->src_addr) {
            int rc = ctx->session_open ? -EPERM : -ENOTCONN;
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return rc;
        }
        if (ctx->reliable) {
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return -ENOTSUP;
        }
        if (hdr->cmd_op == NLK_SHELL_OP_FLOW_CRED && len >= sizeof(uint16_t)) {
            uint16_t add_cred;
            memcpy(&add_cred, data, sizeof(add_cred));
            ctx->rx_credit = (uint16_t)(ctx->rx_credit + add_cred);
            ctx->lease_touched = true;
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return 0;
        }
        if (hdr->cmd_op == NLK_SHELL_STREAM_STDIN) {
            if (len > CONFIG_NLK_SHELL_RX_BUF_SIZE - ctx->rx_count) {
                NLK_SHELL_UNLOCK(ctx, irq_flags);
                return -ENOSPC;
            }
            for (i = 0U; i < len; i++) {
                ctx->rx_buf[ctx->rx_head] = bytes[i];
                ctx->rx_head = (uint16_t)((ctx->rx_head + 1U) % CONFIG_NLK_SHELL_RX_BUF_SIZE);
                ctx->rx_count++;
            }
            ctx->lease_touched = true;
            NLK_SHELL_UNLOCK(ctx, irq_flags);
            return (int)len;
        }
        NLK_SHELL_UNLOCK(ctx, irq_flags);
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
    spin_lock_init(&ctx->io_lock);
    ctx->cols = 80U;
    ctx->rows = 24U;
    ctx->rx_credit = CONFIG_NLK_SHELL_RX_BUF_SIZE;

    rc = nos_tty_init(&ctx->tty, &g_nlk_shell_tty_ops, ctx);
    if (rc < 0) {
        return rc;
    }

    ctx->cap.cap_ver = 0x11U;
    ctx->cap.cap_type = NLK_CAP_SHELL;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_STREAM;
    ctx->cap.sub_class = 0U;
    ctx->cap.desc_size = 0U;
    ctx->cap.name = "nshell_tty";
    ctx->cap.ops = &g_shell_ops;
    ctx->cap.priv = ctx;

    rc = nlk_node_register_cap(node, &ctx->cap);
    if (rc >= 0) __atomic_store_n(&g_default_shell_ctx, ctx, __ATOMIC_RELEASE);
    return rc;
}

struct nos_tty *nlk_cap_shell_tty(struct nlk_shell_ctx *ctx)
{
    return (ctx != NULL) ? &ctx->tty : NULL;
}

struct nlk_shell_ctx *nlk_cap_shell_default_ctx(void)
{
    return __atomic_load_n(&g_default_shell_ctx, __ATOMIC_ACQUIRE);
}

int nlk_cap_shell_flush_pending(struct nlk_shell_ctx *ctx)
{
    uint8_t data[CONFIG_NLK_PACKET_MTU];
    uint16_t count, i, tail;
    uint32_t generation;
    nlk_addr_t addr;
    uint64_t session_id;
    bool reliable;
    unsigned int prefix;
    unsigned long flags;
    int rc;

    if (ctx == NULL || ctx->node == NULL) return -EINVAL;
    flags = NLK_SHELL_LOCK(ctx);
    if (!ctx->session_open || ctx->tx_count == 0U) {
        NLK_SHELL_UNLOCK(ctx, flags);
        return 0;
    }
    reliable = ctx->reliable;
    session_id = ctx->session_id;
    prefix = reliable ? sizeof(session_id) : 0U;
    count = ctx->tx_count;
    if (count > sizeof(data) - prefix) count = (uint16_t)(sizeof(data) - prefix);
    if (prefix != 0U) memcpy(data, &session_id, prefix);
    tail = ctx->tx_tail;
    for (i = 0; i < count; i++) {
        data[prefix + i] = ctx->tx_buf[tail];
        tail = (uint16_t)((tail + 1U) % CONFIG_NLK_SHELL_TX_BUF_SIZE);
    }
    generation = ctx->generation;
    addr = ctx->client_addr;
    NLK_SHELL_UNLOCK(ctx, flags);
    rc = nlk_node_stream_write(ctx->node, addr, ctx->cap.cap_id,
                               reliable ? NLK_SHELL_STREAM_STDOUT_V2 : NLK_SHELL_STREAM_STDOUT,
                               data, (uint16_t)(prefix + count));
    if (rc < 0) return rc;
    flags = NLK_SHELL_LOCK(ctx);
    if (ctx->session_open && generation == ctx->generation) {
        ctx->tx_tail = tail;
        ctx->tx_count -= count;
    }
    NLK_SHELL_UNLOCK(ctx, flags);
    return count;
}

void nlk_cap_shell_tick(struct nlk_shell_ctx *ctx, uint32_t now_ms)
{
    unsigned long flags;
    if (ctx == NULL) return;
    flags = NLK_SHELL_LOCK(ctx);
    if (ctx->session_open) {
        if (ctx->lease_touched || !ctx->lease_started) {
            ctx->lease_time_ms = now_ms;
            ctx->lease_touched = false;
            ctx->lease_started = true;
        } else if ((uint32_t)(now_ms - ctx->lease_time_ms) > NLK_SHELL_LEASE_MS) {
            ctx->session_open = false;
            ctx->prompt_pending = false;
            ctx->rx_head = ctx->rx_tail = ctx->rx_count = 0U;
            ctx->tx_head = ctx->tx_tail = ctx->tx_count = 0U;
            __atomic_add_fetch(&ctx->generation, 1U, __ATOMIC_RELEASE);
        }
    }
    NLK_SHELL_UNLOCK(ctx, flags);
}
