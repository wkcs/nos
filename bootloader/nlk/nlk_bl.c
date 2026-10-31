/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: NLK 协议处理引擎实现
 */

#include "nlk_bl.h"
#include "../drivers/clock.h"
#include "../drivers/flash.h"
#include "../drivers/rgb.h"
#include "../drivers/uart.h"
#include <boot_proto.h>
#include <stm32f4xx.h>

#define BL_VERSION_CODE \
    (((uint32_t)(BL_VER_MAJOR) << 16U) | ((uint32_t)(BL_VER_MINOR) << 8U) | (uint32_t)(BL_VER_PATCH))

static struct {
    bool     active;
    bool     reboot_pending;
    uint32_t reboot_flag;
    uint32_t total_size;
    uint32_t expected_crc32;
    uint32_t running_crc32;
    uint32_t acked_offset;
    uint32_t first_word;
} g_bl_flash;

static void bl_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0U) {
        *d++ = *s++;
    }
}

static void bl_memset(void *dst, uint8_t val, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0U) {
        *d++ = val;
    }
}

uint8_t nlk_crc8(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t crc = 0x00U;
    uint32_t i;
    uint8_t b;

    for (i = 0U; i < len; i++) {
        crc ^= p[i];
        for (b = 0U; b < 8U; b++) {
            crc = ((crc & 0x80U) != 0U) ? (uint8_t)((crc << 1U) ^ 0x07U) : (uint8_t)(crc << 1U);
        }
    }
    return crc;
}

uint16_t nlk_crc16(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFFU;
    uint32_t i;
    uint8_t b;

    for (i = 0U; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8U);
        for (b = 0U; b < 8U; b++) {
            crc = ((crc & 0x8000U) != 0U) ? (uint16_t)((crc << 1U) ^ 0x1021U) : (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

uint32_t nlk_crc32(uint32_t init_crc, const void *data, uint32_t len)
{
    static const uint32_t crc_nibble[16] = {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL,
    };
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~init_crc;
    uint32_t i;

    for (i = 0U; i < len; i++) {
        crc = (crc >> 4U) ^ crc_nibble[(crc ^ p[i]) & 0x0FU];
        crc = (crc >> 4U) ^ crc_nibble[(crc ^ (p[i] >> 4U)) & 0x0FU];
    }
    return ~crc;
}

void nlk_bl_init(void)
{
    bl_memset(&g_bl_flash, 0, sizeof(g_bl_flash));
}

bool nlk_bl_is_reboot_pending(uint32_t *out_flag)
{
    if (g_bl_flash.reboot_pending) {
        if (out_flag != NULL) {
            *out_flag = g_bl_flash.reboot_flag;
        }
        return true;
    }
    return false;
}

void nlk_bl_trigger_reboot(uint32_t flag)
{
    /* 写入目标启动标志并触发系统复位 */
    backup_write_flag(flag);
    delay_ms(20U);
    NVIC_SystemReset();
}

static int bl_kernel_prepare(uint32_t total_size, uint32_t expected_crc32)
{
    static const uint32_t sec_end_offsets[5] = {
        16UL * 1024UL,   /* Sector 1: 16 KB */
        32UL * 1024UL,   /* Sector 2: 16 KB */
        48UL * 1024UL,   /* Sector 3: 16 KB */
        112UL * 1024UL,  /* Sector 4: 64 KB */
        240UL * 1024UL,  /* Sector 5: 128 KB */
    };
    uint32_t s;

    if (total_size < 64UL || total_size > NK60_KERNEL_MAX_BIN) {
        return -27; /* -EFBIG */
    }

    /* 亮起蓝灯指示擦除开始 */
    rgb_set_solid(0U, 0U, 255U);
    flash_unlock();
    for (s = 0UL; s < 5UL; s++) {
        uint32_t prev_end = (s == 0UL) ? 0UL : sec_end_offsets[s - 1UL];
        if (total_size > prev_end) {
            if (flash_erase_sector(s + 1UL) != 0) {
                flash_lock_and_flush();
                return -5; /* -EIO */
            }
        }
    }
    flash_lock_and_flush();

    g_bl_flash.active = true;
    g_bl_flash.total_size = total_size;
    g_bl_flash.expected_crc32 = expected_crc32;
    g_bl_flash.running_crc32 = 0UL;
    g_bl_flash.acked_offset = 0UL;
    g_bl_flash.first_word = 0xFFFFFFFFUL;
    return 0;
}

static int bl_kernel_write_chunk(uint32_t offset, const uint8_t *data, uint16_t len,
                                 struct nlk_flash_chunk_ack *ack)
{
    bl_memset(ack, 0, sizeof(*ack));
    if (!g_bl_flash.active) {
        ack->status = -1;
        return -1;
    }
    if (offset < g_bl_flash.acked_offset) {
        ack->acked_offset = g_bl_flash.acked_offset;
        ack->rx_window_bitmap = 1U;
        return 0;
    }
    if (offset != g_bl_flash.acked_offset ||
        offset + (uint32_t)len > g_bl_flash.total_size) {
        ack->acked_offset = g_bl_flash.acked_offset;
        ack->status = -22;
        return -22;
    }

    if (offset == 0UL && len >= 8U) {
        uint32_t msp;
        uint32_t reset_pc;

        bl_memcpy(&msp, data, 4U);
        bl_memcpy(&reset_pc, data + 4U, 4U);
        if (!nk60_validate_cortexm_vectors(msp, reset_pc,
                                           NK60_KERNEL_FLASH_BASE,
                                           NK60_INT_FLASH_END)) {
            ack->status = -74; /* -EBADMSG */
            return -74;
        }
        g_bl_flash.first_word = msp;
    }

    flash_unlock();
    if (offset == 0UL && len > 4U) {
        /* 首字 (MSP) 暂存于 RAM，先从偏移 4 开始写入内部 Flash 以保证掉电安全 */
        if (flash_program_words(NK60_KERNEL_FLASH_BASE + 4UL,
                                data + 4U, (uint32_t)(len - 4U)) != 0) {
            flash_lock_and_flush();
            ack->status = -5;
            return -5;
        }
    } else if (offset > 0UL && len > 0U) {
        if (flash_program_words(NK60_KERNEL_FLASH_BASE + offset,
                                data, len) != 0) {
            flash_lock_and_flush();
            ack->status = -5;
            return -5;
        }
    }
    flash_lock_and_flush();

    g_bl_flash.running_crc32 = nlk_crc32(g_bl_flash.running_crc32, data, len);
    g_bl_flash.acked_offset += len;
    ack->acked_offset = g_bl_flash.acked_offset;
    ack->rx_window_bitmap = 1U;
    ack->status = 0;
    return 0;
}

static int bl_kernel_commit(void)
{
    uint32_t int_crc;

    if (!g_bl_flash.active || g_bl_flash.acked_offset != g_bl_flash.total_size) {
        return -22;
    }

    flash_lock_and_flush();
    int_crc = nlk_crc32(0UL, &g_bl_flash.first_word, sizeof(g_bl_flash.first_word));
    if (g_bl_flash.total_size > 4UL) {
        int_crc = nlk_crc32(int_crc,
                            (const void *)(uintptr_t)(NK60_KERNEL_FLASH_BASE + 4UL),
                            g_bl_flash.total_size - 4UL);
    }

    if ((g_bl_flash.expected_crc32 != 0UL && int_crc != g_bl_flash.expected_crc32) ||
        int_crc != g_bl_flash.running_crc32) {
        g_bl_flash.active = false;
        rgb_set_solid(255U, 0U, 0U); /* 红灯报警 */
        return -74; /* -EBADMSG */
    }

    /* 全文 CRC32 校验通过后原子写入首字 MSP (0x08004000) */
    flash_unlock();
    if (flash_program_words(NK60_KERNEL_FLASH_BASE,
                            (const uint8_t *)&g_bl_flash.first_word,
                            sizeof(g_bl_flash.first_word)) != 0) {
        flash_lock_and_flush();
        return -5;
    }
    flash_lock_and_flush();

    g_bl_flash.active = false;
    rgb_set_solid(0U, 255U, 0U); /* 绿灯成功 */
    uart_puts("[BL] Kernel flashed to internal Flash OK\r\n");
    return 0;
}

static void bl_fill_flash_query(struct nlk_flash_query_resp *q)
{
    bl_memset(q, 0, sizeof(*q));
    q->part_count = 2U;
    q->run_mode = BOOT_BOOTLOADER;
    q->max_chunk_size = 256U;
    q->running_version = BL_VERSION_CODE;

    q->parts[0].part_id = 0U;
    q->parts[0].flags = 0U; /* Bootloader 自身在 BL 模式下只读 */
    q->parts[0].max_size = NK60_BOOTLOADER_MAX_BIN;
    q->parts[0].erase_size = NK60_BOOTLOADER_MAX_BIN;
    bl_memcpy(q->parts[0].name, "bootloader", 11U);

    q->parts[1].part_id = 1U;
    q->parts[1].flags = NLK_FLASH_PART_F_WRITABLE | NLK_FLASH_PART_F_REBOOT_AFTER;
    q->parts[1].max_size = NK60_KERNEL_MAX_BIN;
    q->parts[1].erase_size = 16UL * 1024UL;
    bl_memcpy(q->parts[1].name, "kernel", 7U);
}

uint16_t nlk_bl_handle_packet(const uint8_t *rx_pkt, uint16_t rx_len, uint8_t *tx_pkt)
{
    const struct nlk_pkt_hdr *in_hdr = (const struct nlk_pkt_hdr *)rx_pkt;
    const uint8_t *payload = rx_pkt + sizeof(*in_hdr);
    uint16_t payload_len;
    struct nlk_pkt_hdr *out_hdr = (struct nlk_pkt_hdr *)tx_pkt;
    uint8_t *out_pay = tx_pkt + sizeof(*out_hdr);
    uint16_t out_len = 0U;
    int rc = 0;
    uint8_t mode;

    if (rx_len < sizeof(*in_hdr)) {
        return 0U;
    }
    payload_len = in_hdr->payload_len;
    if ((uint32_t)sizeof(*in_hdr) + payload_len > rx_len) {
        return 0U;
    }
    mode = (uint8_t)((in_hdr->msg_type >> 4U) & 0x0FU);

    if (in_hdr->dst_cap == NLK_CAP_ID_CORE) {
        switch (in_hdr->cmd_op) {
        case NLK_CORE_OP_GET_DEV_DESC: {
            struct nlk_device_desc *d = (struct nlk_device_desc *)out_pay;
            bl_memset(d, 0, sizeof(*d));
            d->vendor_id = 0x4E4BU;
            d->product_id = 0x0060U;
            d->device_class = 0x0010U;
            d->hw_ver[0] = 2U;
            d->fw_ver[0] = (uint8_t)BL_VER_MAJOR;
            d->fw_ver[1] = (uint8_t)BL_VER_MINOR;
            d->fw_ver[2] = (uint8_t)BL_VER_PATCH;
            bl_memcpy(d->name, "NK60-V2-BL", 11U);
            d->max_payload_size = BL_NLK_MTU;
            d->cap_count = 1U;
            out_len = sizeof(*d);
            break;
        }
        case NLK_CORE_OP_GET_CAP_LIST: {
            struct nlk_cap_item *item = (struct nlk_cap_item *)out_pay;
            bl_memset(item, 0, sizeof(*item));
            item->cap_id = NLK_CAP_ID_FLASH;
            item->cap_ver = 0x10U;
            item->cap_type = NLK_CAP_FLASH;
            item->sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_BULK;
            item->desc_size = sizeof(struct nlk_flash_query_resp);
            out_len = sizeof(*item);
            break;
        }
        case NLK_CORE_OP_GET_CAP_DESC:
            bl_fill_flash_query((struct nlk_flash_query_resp *)out_pay);
            out_len = sizeof(struct nlk_flash_query_resp);
            break;
        case NLK_CORE_OP_GET_TOPOLOGY: {
            struct nlk_topo_node_entry *node = (struct nlk_topo_node_entry *)out_pay;
            bl_memset(node, 0, sizeof(*node));
            node->node_addr = 0x0001U;
            node->parent_addr = 0x0000U;
            node->vendor_id = 0x4E4BU;
            node->product_id = 0x0060U;
            node->device_class = 0x0010U;
            node->cap_count = 1U;
            node->port_id = 0U;
            out_len = sizeof(*node);
            break;
        }
        case NLK_CORE_OP_PING:
            bl_memcpy(out_pay, payload, payload_len);
            out_len = payload_len;
            break;
        case NLK_CORE_OP_RESET:
            g_bl_flash.active = false;
            break;
        case NLK_CORE_OP_REBOOT: {
            uint32_t flag = BOOT_NORMAL;
            if (payload_len >= sizeof(struct nlk_core_reboot_req)) {
                bl_memcpy(&flag, payload, sizeof(flag));
            }
            g_bl_flash.reboot_pending = true;
            g_bl_flash.reboot_flag = flag;
            break;
        }
        default:
            rc = -95;
            break;
        }
    } else if (in_hdr->dst_cap == NLK_CAP_ID_FLASH) {
        if (mode == NLK_MODE_BULK || in_hdr->cmd_op == NLK_FLASH_OP_WRITE_CHUNK) {
            struct nlk_flash_chunk_hdr chdr;
            struct nlk_flash_chunk_ack *ack = (struct nlk_flash_chunk_ack *)out_pay;
            if (payload_len < sizeof(chdr)) {
                rc = -22;
            } else {
                bl_memcpy(&chdr, payload, sizeof(chdr));
                rc = bl_kernel_write_chunk(chdr.offset,
                                           payload + sizeof(chdr),
                                           (uint16_t)(payload_len - sizeof(chdr)),
                                           ack);
                out_len = sizeof(*ack);
            }
        } else {
            switch (in_hdr->cmd_op) {
            case NLK_FLASH_OP_QUERY_PARTS:
                bl_fill_flash_query((struct nlk_flash_query_resp *)out_pay);
                out_len = sizeof(struct nlk_flash_query_resp);
                break;
            case NLK_FLASH_OP_PREPARE: {
                const struct nlk_flash_prepare_req *prep =
                    (const struct nlk_flash_prepare_req *)payload;
                if (payload_len < sizeof(*prep) || prep->part_id != 1U) {
                    rc = -22;
                } else {
                    rc = bl_kernel_prepare(prep->total_size, prep->expected_crc32);
                }
                break;
            }
            case NLK_FLASH_OP_COMMIT:
                rc = bl_kernel_commit();
                if (rc == 0) {
                    bl_memcpy(out_pay, &g_bl_flash.running_crc32, 4U);
                    out_len = 4U;
                }
                break;
            case NLK_FLASH_OP_ABORT:
                g_bl_flash.active = false;
                break;
            default:
                rc = -95;
                break;
            }
        }
    } else {
        rc = -2;
    }

    if ((in_hdr->ver_flags & NLK_FLAG_ACK_REQ) == 0U) {
        return 0U;
    }

    out_hdr->ver_flags = (uint8_t)(NLK_PROTO_VER_1 | NLK_FLAG_IS_ACK |
                                   ((rc < 0) ? NLK_FLAG_IS_ERR : 0U));
    out_hdr->msg_type = in_hdr->msg_type;
    out_hdr->src_addr = in_hdr->dst_addr;
    out_hdr->dst_addr = in_hdr->src_addr;
    out_hdr->dst_cap = in_hdr->dst_cap;
    out_hdr->cmd_op = in_hdr->cmd_op;
    out_hdr->seq_id = in_hdr->seq_id;

    if (rc < 0) {
        int16_t err16 = (int16_t)rc;
        bl_memcpy(out_pay, &err16, sizeof(err16));
        out_len = sizeof(err16);
    }
    out_hdr->payload_len = out_len;
    return (uint16_t)(sizeof(*out_hdr) + out_len);
}
