/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK Capability 0x0001: Firmware Update (OTA / DFU) with Sliding Window
 * & Cascaded Sub-Device Support (100% Hardware-Independent).
 */

#include <nos/nlk_caps.h>
#ifndef NLK_HOST_TEST
#include <kernel/printk.h>
#else
#define pr_info(fmt, ...) do { (void)(fmt); } while (0)
#define pr_err(fmt, ...)  do { (void)(fmt); } while (0)
#endif
#include <uapi/nos/errno.h>
#include <string.h>

static int fw_update_get_desc(struct nlk_node *node, struct nlk_capability *cap,
                              void *buf, uint16_t max_len)
{
    struct nlk_fw_update_ctx *ctx = (struct nlk_fw_update_ctx *)cap->priv;
    struct nlk_fw_part_info_resp info;
    uint8_t i;

    (void)node;
    if (buf == NULL || max_len < sizeof(info)) {
        return -ENOSPC;
    }
    memset(&info, 0, sizeof(info));
    info.active_part = ctx->active_part;
    info.part_count = ctx->part_count;
    info.max_chunk_size = (uint16_t)(CONFIG_NLK_PACKET_MTU - sizeof(struct nlk_fw_chunk_hdr));
    info.max_window_pkts = ctx->max_window_pkts;
    info.running_version = ctx->running_version;
    for (i = 0U; i < NLK_FW_MAX_PARTS; i++) {
        info.part_max_size[i] = ctx->part_max_size[i];
    }

    memcpy(buf, &info, sizeof(info));
    return (int)sizeof(info);
}

static int fw_update_on_rpc(struct nlk_node *node, struct nlk_capability *cap,
                            const struct nlk_pkt_hdr *hdr,
                            const void *req, uint16_t req_len,
                            void *resp, uint16_t *resp_len)
{
    struct nlk_fw_update_ctx *ctx = (struct nlk_fw_update_ctx *)cap->priv;
    int rc;

    if (ctx == NULL || resp_len == NULL) {
        return -EINVAL;
    }
    *resp_len = 0U;

    switch (hdr->cmd_op) {
    case NLK_FW_OP_GET_PART_INFO:
        rc = fw_update_get_desc(node, cap, resp, CONFIG_NLK_PACKET_MTU);
        if (rc < 0) {
            return rc;
        }
        *resp_len = (uint16_t)rc;
        return 0;

    case NLK_FW_OP_PREPARE: {
        const struct nlk_fw_prepare_req *prep = (const struct nlk_fw_prepare_req *)req;
        uint32_t max_part_bytes;

        if (req == NULL || req_len < sizeof(*prep)) {
            return -EINVAL;
        }
        if (prep->part_index >= ctx->part_count ||
            prep->part_index >= NLK_FW_MAX_PARTS ||
            prep->total_size == 0U) {
            return -EINVAL;
        }
        max_part_bytes = ctx->part_max_size[prep->part_index];
        if (max_part_bytes > 0U && prep->total_size > max_part_bytes) {
            return -EFBIG;
        }
        if (ctx->flash_prepare != NULL) {
            rc = ctx->flash_prepare(prep->part_index, prep->total_size, ctx->flash_priv);
            if (rc < 0) {
                return rc;
            }
        }

        ctx->session_active = true;
        ctx->target_part = prep->part_index;
        ctx->window_pkts = (prep->window_pkts > 0U && prep->window_pkts <= 16U) ?
                           prep->window_pkts : 1U;
        ctx->rx_window_bitmap = 0U;
        ctx->total_size = prep->total_size;
        ctx->expected_crc32 = prep->image_crc32;
        ctx->running_crc32 = 0U;
        ctx->acked_offset = 0U;
        pr_info("[NLK-FW] prepare: part=%u, total_size=%u, crc32=0x%08x\r\n",
                (unsigned int)ctx->target_part, (unsigned int)ctx->total_size,
                (unsigned int)ctx->expected_crc32);
        return 0;
    }

    case NLK_FW_OP_VERIFY_COMMIT:
        if (!ctx->session_active) {
            return -EPROTO;
        }
        if (ctx->acked_offset != ctx->total_size) {
            return -EAGAIN;
        }
        if (ctx->running_crc32 != ctx->expected_crc32) {
            pr_err("[NLK-FW] verify failed: calc_crc=0x%08x != exp_crc=0x%08x\r\n",
                   (unsigned int)ctx->running_crc32,
                   (unsigned int)ctx->expected_crc32);
            ctx->session_active = false;
            return -EBADMSG;
        }
        if (ctx->flash_commit != NULL) {
            rc = ctx->flash_commit(ctx->target_part, ctx->total_size,
                                   ctx->running_crc32, ctx->flash_priv);
            if (rc < 0) {
                return rc;
            }
        }
        pr_info("[NLK-FW] verify & commit ok: part=%u, size=%u, crc32=0x%08x\r\n",
                (unsigned int)ctx->target_part, (unsigned int)ctx->total_size,
                (unsigned int)ctx->running_crc32);
        ctx->active_part = ctx->target_part;
        ctx->session_active = false;
        return 0;

    case NLK_FW_OP_REBOOT:
        ctx->session_active = false;
        if (ctx->flash_reboot != NULL) {
            ctx->flash_reboot(ctx->active_part, ctx->flash_priv);
        }
        return 0;

    default:
        return -ENOTSUP;
    }
}

static int fw_update_on_bulk(struct nlk_node *node, struct nlk_capability *cap,
                             const struct nlk_pkt_hdr *hdr,
                             uint32_t offset, const void *data, uint16_t len,
                             void *ack_buf, uint16_t *ack_len)
{
    struct nlk_fw_update_ctx *ctx = (struct nlk_fw_update_ctx *)cap->priv;
    struct nlk_fw_chunk_ack *ack = (struct nlk_fw_chunk_ack *)ack_buf;
    uint32_t prev_step;
    uint32_t curr_step;
    int rc = 0;

    (void)node;
    if (ctx == NULL || ack == NULL || ack_len == NULL ||
        hdr->cmd_op != NLK_FW_OP_WRITE_CHUNK) {
        return -EINVAL;
    }
    if (!ctx->session_active) {
        return -EPROTO;
    }

    /* Idempotent duplicate chunk handling (e.g., ACK lost on wire) */
    if (offset < ctx->acked_offset) {
        ack->acked_offset = ctx->acked_offset;
        ack->rx_window_bitmap = ctx->rx_window_bitmap;
        ack->status = 0;
        *ack_len = sizeof(*ack);
        return 0;
    }

    /* Out-of-order chunk gap */
    if (offset > ctx->acked_offset) {
        ack->acked_offset = ctx->acked_offset;
        ack->rx_window_bitmap = ctx->rx_window_bitmap;
        ack->status = -EAGAIN;
        *ack_len = sizeof(*ack);
        return 0;
    }

    if (offset + len > ctx->total_size) {
        return -EFBIG;
    }

    if (ctx->flash_write != NULL && len > 0U) {
        rc = ctx->flash_write(ctx->target_part, offset,
                              (const uint8_t *)data, len, ctx->flash_priv);
        if (rc < 0) {
            ack->acked_offset = ctx->acked_offset;
            ack->rx_window_bitmap = ctx->rx_window_bitmap;
            ack->status = (int16_t)rc;
            *ack_len = sizeof(*ack);
            return rc;
        }
    }

    prev_step = ctx->acked_offset / 8192U;
    ctx->running_crc32 = nlk_crc32(ctx->running_crc32, data, len);
    ctx->acked_offset += len;
    curr_step = ctx->acked_offset / 8192U;
    if (offset == 0U || curr_step > prev_step ||
        ctx->acked_offset == ctx->total_size) {
        uint32_t pct = (ctx->total_size > 0U) ?
                       (uint32_t)(((uint64_t)ctx->acked_offset * 100ULL) / ctx->total_size) :
                       100U;
        (void)pct;
        pr_info("[NLK-FW] recv chunk: %u/%u bytes (%u%%)\r\n",
                (unsigned int)ctx->acked_offset,
                (unsigned int)ctx->total_size,
                (unsigned int)pct);
    }

    ctx->rx_window_bitmap = (uint16_t)((ctx->rx_window_bitmap << 1U) | 1U);

    ack->acked_offset = ctx->acked_offset;
    ack->rx_window_bitmap = ctx->rx_window_bitmap;
    ack->status = 0;
    *ack_len = sizeof(*ack);
    return 0;
}

static const struct nlk_cap_ops g_fw_update_ops = {
    .get_desc = fw_update_get_desc,
    .on_rpc   = fw_update_on_rpc,
    .on_bulk  = fw_update_on_bulk,
};

int nlk_cap_fw_update_init(struct nlk_node *node, struct nlk_fw_update_ctx *ctx,
                           uint32_t running_version,
                           nlk_flash_write_fn write_fn,
                           nlk_flash_commit_fn commit_fn,
                           void *flash_priv)
{
    if (node == NULL || ctx == NULL) {
        return -EINVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->part_count = NLK_FW_MAX_PARTS;
    ctx->active_part = NLK_FW_PART_SLOT1;
    ctx->max_window_pkts = 8U;
    ctx->running_version = running_version;
    ctx->flash_write = write_fn;
    ctx->flash_commit = commit_fn;
    ctx->flash_priv = flash_priv;

    ctx->cap.cap_ver = 0x10U;
    ctx->cap.cap_type = NLK_CAP_FW_UPDATE;
    ctx->cap.sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_BULK;
    ctx->cap.sub_class = 0U;
    ctx->cap.desc_size = sizeof(struct nlk_fw_part_info_resp);
    ctx->cap.name = "fw_update";
    ctx->cap.ops = &g_fw_update_ops;
    ctx->cap.priv = ctx;

    return nlk_node_register_cap(node, &ctx->cap);
}

int nlk_cap_fw_update_set_partitions(struct nlk_fw_update_ctx *ctx,
                                     uint8_t part_count, uint8_t active_part,
                                     const uint32_t *part_max_sizes)
{
    uint8_t i;

    if (ctx == NULL || part_count == 0U || part_count > NLK_FW_MAX_PARTS ||
        active_part >= part_count) {
        return -EINVAL;
    }
    ctx->part_count = part_count;
    ctx->active_part = active_part;
    memset(ctx->part_max_size, 0, sizeof(ctx->part_max_size));
    if (part_max_sizes != NULL) {
        for (i = 0U; i < part_count; i++) {
            ctx->part_max_size[i] = part_max_sizes[i];
        }
    }
    return 0;
}
