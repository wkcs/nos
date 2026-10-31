/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0001: NLK_CAP_FLASH
 * 通用分区表注册与流式固件/分区刷写引擎（支持原始二进制流、增量 CRC32 校验与按名寻址）。
 */

#include <nos/nlk_caps.h>
#include <uapi/nos/errno.h>
#include <string.h>

static int cap_flash_fill_query_resp(struct nlk_flash_ctx *ctx,
                                     struct nlk_flash_query_resp *info)
{
    uint8_t i;

    if (ctx == NULL || info == NULL) {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));
    info->part_count = ctx->part_count;
    info->run_mode = ctx->run_mode;
    info->max_chunk_size = (uint16_t)(CONFIG_NLK_PACKET_MTU -
                                      sizeof(struct nlk_flash_chunk_hdr));
    info->running_version = ctx->running_version;

    for (i = 0U; i < ctx->part_count && i < NLK_FLASH_MAX_PARTS; i++) {
        const struct nlk_flash_part_ops *src = &ctx->parts[i];
        struct nlk_flash_part_desc *dst = &info->parts[i];

        dst->part_id = i;
        dst->flags = src->flags;
        dst->max_size = src->max_size;
        dst->erase_size = src->erase_size;
        if (src->name != NULL) {
            size_t nlen = strlen(src->name);
            if (nlen >= NLK_FLASH_PART_NAME_MAX) {
                nlen = NLK_FLASH_PART_NAME_MAX - 1U;
            }
            memcpy(dst->name, src->name, nlen);
            dst->name[nlen] = '\0';
        }
    }

    return (int)sizeof(*info);
}

static int cap_flash_get_desc(struct nlk_node *node, struct nlk_capability *cap,
                              void *buf, uint16_t max_len)
{
    struct nlk_flash_ctx *ctx = (struct nlk_flash_ctx *)cap->priv;
    struct nlk_flash_query_resp info;

    (void)node;
    if (ctx == NULL || buf == NULL || max_len < sizeof(info)) {
        return -EINVAL;
    }
    (void)cap_flash_fill_query_resp(ctx, &info);
    memcpy(buf, &info, sizeof(info));
    return (int)sizeof(info);
}

static int cap_flash_write_chunk_internal(struct nlk_flash_ctx *ctx,
                                          uint32_t offset,
                                          const uint8_t *data, uint16_t len,
                                          struct nlk_flash_chunk_ack *ack)
{
    const struct nlk_flash_part_ops *part;
    int rc;

    if (ack != NULL) {
        memset(ack, 0, sizeof(*ack));
    }

    if (ctx == NULL || !ctx->session_active ||
        ctx->target_part >= ctx->part_count) {
        if (ack != NULL) {
            ack->status = -EPERM;
        }
        return -EPERM;
    }

    part = &ctx->parts[ctx->target_part];

    /* 幂等重传：若主机重发已确认过的相同末尾块，直接返回当前进度 */
    if (offset < ctx->acked_offset) {
        if (ack != NULL) {
            ack->acked_offset = ctx->acked_offset;
            ack->rx_window_bitmap = 1U;
            ack->status = 0;
        }
        return 0;
    }

    if (offset != ctx->acked_offset) {
        if (ack != NULL) {
            ack->acked_offset = ctx->acked_offset;
            ack->rx_window_bitmap = 0U;
            ack->status = -EAGAIN;
        }
        return -EAGAIN;
    }

    if (offset + (uint32_t)len > ctx->total_size) {
        if (ack != NULL) {
            ack->acked_offset = ctx->acked_offset;
            ack->status = -EFBIG;
        }
        return -EFBIG;
    }

    if (part->write != NULL && len > 0U) {
        rc = part->write(offset, data, len, part->priv);
        if (rc < 0) {
            if (ack != NULL) {
                ack->acked_offset = ctx->acked_offset;
                ack->status = (int16_t)rc;
            }
            return rc;
        }
    }

    ctx->running_crc32 = nlk_crc32(ctx->running_crc32, data, len);
    ctx->acked_offset += len;

    if (ack != NULL) {
        ack->acked_offset = ctx->acked_offset;
        ack->rx_window_bitmap = 1U;
        ack->status = 0;
    }
    return 0;
}

static int cap_flash_on_rpc(struct nlk_node *node, struct nlk_capability *cap,
                            const struct nlk_pkt_hdr *hdr,
                            const void *req, uint16_t req_len,
                            void *resp, uint16_t *resp_len)
{
    struct nlk_flash_ctx *ctx = (struct nlk_flash_ctx *)cap->priv;
    int rc;

    if (ctx == NULL || hdr == NULL || resp_len == NULL) {
        return -EINVAL;
    }
    *resp_len = 0U;

    switch (hdr->cmd_op) {
    case NLK_FLASH_OP_QUERY_PARTS:
        rc = cap_flash_get_desc(node, cap, resp, CONFIG_NLK_PACKET_MTU);
        if (rc < 0) {
            return rc;
        }
        *resp_len = (uint16_t)rc;
        return 0;

    case NLK_FLASH_OP_PREPARE: {
        const struct nlk_flash_prepare_req *prep =
            (const struct nlk_flash_prepare_req *)req;
        const struct nlk_flash_part_ops *part;

        if (req == NULL || req_len < sizeof(*prep)) {
            return -EINVAL;
        }
        if (prep->part_id >= ctx->part_count) {
            return -ENOENT;
        }

        part = &ctx->parts[prep->part_id];
        if ((part->flags & NLK_FLASH_PART_F_WRITABLE) == 0U) {
            return -EACCES;
        }
        if ((part->flags & NLK_FLASH_PART_F_NEEDS_BL) != 0U) {
            /* 当前分区在运行态下不可直接覆写，需先切入 Bootloader 模式 */
            return -EBUSY;
        }
        if (prep->total_size == 0UL || prep->total_size > part->max_size) {
            return -EFBIG;
        }

        if (part->prepare != NULL) {
            rc = part->prepare(prep->total_size, prep->expected_crc32, part->priv);
            if (rc < 0) {
                return rc;
            }
        }

        ctx->session_active = true;
        ctx->target_part = prep->part_id;
        ctx->total_size = prep->total_size;
        ctx->expected_crc32 = prep->expected_crc32;
        ctx->running_crc32 = 0U;
        ctx->acked_offset = 0U;
        return 0;
    }

    case NLK_FLASH_OP_WRITE_CHUNK: {
        const struct nlk_flash_chunk_hdr *chdr =
            (const struct nlk_flash_chunk_hdr *)req;
        struct nlk_flash_chunk_ack *ack = (struct nlk_flash_chunk_ack *)resp;

        if (req == NULL || req_len < sizeof(*chdr) || resp == NULL) {
            return -EINVAL;
        }
        rc = cap_flash_write_chunk_internal(
            ctx, chdr->offset,
            (const uint8_t *)req + sizeof(*chdr),
            (uint16_t)(req_len - sizeof(*chdr)), ack);
        *resp_len = sizeof(*ack);
        return rc;
    }

    case NLK_FLASH_OP_COMMIT: {
        const struct nlk_flash_part_ops *part;

        if (!ctx->session_active || ctx->target_part >= ctx->part_count) {
            return -EPERM;
        }
        if (req != NULL && req_len >= sizeof(struct nlk_flash_commit_req)) {
            const struct nlk_flash_commit_req *creq =
                (const struct nlk_flash_commit_req *)req;
            if (creq->part_id != ctx->target_part ||
                creq->total_size != ctx->total_size) {
                ctx->session_active = false;
                return -EINVAL;
            }
            if (creq->expected_crc32 != 0U) {
                ctx->expected_crc32 = creq->expected_crc32;
            }
        }

        if (ctx->acked_offset != ctx->total_size) {
            return -EAGAIN;
        }
        if (ctx->expected_crc32 != 0U &&
            ctx->running_crc32 != ctx->expected_crc32) {
            ctx->session_active = false;
            return -EBADMSG;
        }

        part = &ctx->parts[ctx->target_part];
        if (part->commit != NULL) {
            rc = part->commit(ctx->total_size, ctx->running_crc32, part->priv);
            if (rc < 0) {
                ctx->session_active = false;
                return rc;
            }
        }

        ctx->session_active = false;
        if (resp != NULL) {
            memcpy(resp, &ctx->running_crc32, sizeof(ctx->running_crc32));
            *resp_len = sizeof(ctx->running_crc32);
        }
        return 0;
    }

    case NLK_FLASH_OP_ABORT:
        ctx->session_active = false;
        return 0;

    default:
        return -ENOTSUP;
    }
}

static int cap_flash_on_bulk(struct nlk_node *node, struct nlk_capability *cap,
                             const struct nlk_pkt_hdr *hdr,
                             uint32_t offset, const void *data, uint16_t len,
                             void *ack_buf, uint16_t *ack_len)
{
    struct nlk_flash_ctx *ctx = (struct nlk_flash_ctx *)cap->priv;
    struct nlk_flash_chunk_ack *ack = (struct nlk_flash_chunk_ack *)ack_buf;
    int rc;

    (void)node;
    (void)hdr;
    if (ctx == NULL || ack == NULL || ack_len == NULL) {
        return -EINVAL;
    }
    *ack_len = sizeof(*ack);
    rc = cap_flash_write_chunk_internal(ctx, offset,
                                        (const uint8_t *)data, len, ack);
    return rc;
}

static const struct nlk_cap_ops g_cap_flash_ops = {
    .get_desc = cap_flash_get_desc,
    .on_rpc   = cap_flash_on_rpc,
    .on_bulk  = cap_flash_on_bulk,
};

int nlk_cap_flash_init(struct nlk_node *node, struct nlk_flash_ctx *ctx,
                       uint8_t run_mode, uint32_t running_version)
{
    if (node == NULL || ctx == NULL) {
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->run_mode = run_mode;
    ctx->running_version = running_version;

    ctx->cap.cap_ver = 0x10U;
    ctx->cap.cap_type = NLK_CAP_FLASH;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_BULK;
    ctx->cap.desc_size = sizeof(struct nlk_flash_query_resp);
    ctx->cap.name = "flash";
    ctx->cap.ops = &g_cap_flash_ops;
    ctx->cap.priv = ctx;

    return nlk_node_register_cap(node, &ctx->cap);
}

int nlk_cap_flash_register_part(struct nlk_flash_ctx *ctx,
                                const struct nlk_flash_part_ops *part)
{
    uint8_t idx;

    if (ctx == NULL || part == NULL || part->name == NULL ||
        part->max_size == 0UL) {
        return -EINVAL;
    }
    if (ctx->part_count >= NLK_FLASH_MAX_PARTS) {
        return -ENOSPC;
    }

    idx = ctx->part_count++;
    ctx->parts[idx] = *part;
    return (int)idx;
}

