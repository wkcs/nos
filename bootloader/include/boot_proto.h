/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 独立 Bootloader 与 NLK Flash 协议共享定义。
 * 与 NOS 内核 `include/uapi/nos/nlk.h`、`include/kernel/boot.h` 保持二进制 100% 一致。
 */

#ifndef __NK60_BOOT_PROTO_H__
#define __NK60_BOOT_PROTO_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* STM32F401CCU6 片上 Flash 与 SRAM 物理几何参数 */
#define NK60_INT_FLASH_BASE         0x08000000UL
#define NK60_INT_FLASH_END          0x08040000UL
#define NK60_BOOTLOADER_FLASH_BASE  0x08000000UL
#define NK60_BOOTLOADER_MAX_BIN     (16UL * 1024UL)
#define NK60_KERNEL_FLASH_BASE      0x08004000UL
#define NK60_KERNEL_MAX_BIN         (240UL * 1024UL)
#define NK60_SRAM_BASE              0x20000000UL
#define NK60_SRAM_END               0x20010000UL

/* 内核与 Bootloader 共享的启动标志编码（RTC->BKP0R，与 include/kernel/boot.h 一致） */
#define BOOT_FLAG_MAGIC             0x20300000UL
#define BOOT_FLAG_MAGIC_MASK        0xFFFF0000UL
#define BOOT_FLAG_MASK              0x0000FFFFUL

#define get_boot_flag(data)         ((uint32_t)(BOOT_FLAG_MASK & (uint32_t)(data)))
#define get_boot_flag_magic(data)   ((uint32_t)(BOOT_FLAG_MAGIC_MASK & (uint32_t)(data)))
#define get_boot_flag_data(flag)    ((uint32_t)(((uint32_t)(flag) & BOOT_FLAG_MASK) | BOOT_FLAG_MAGIC))

enum boot_flag {
    BOOT_NORMAL     = 0,
    BOOT_BOOTLOADER = 1,
    BOOT_RECOVERY   = 2,
};

/* NLK L2/L3/L4 协议核心常量与头部（与 include/uapi/nos/nlk.h 一致） */
#define NLK_PROTO_VER_1             (1U << 5)
#define NLK_PROTO_VER_MASK          0xE0U
#define NLK_SOF_BYTE0               0x55U
#define NLK_SOF_BYTE1               0xAAU

#define NLK_FLAG_NONE               0x00U
#define NLK_FLAG_ACK_REQ            (1U << 0)
#define NLK_FLAG_IS_ACK             (1U << 1)
#define NLK_FLAG_IS_ERR             (1U << 2)
#define NLK_FLAG_MASK               0x1FU

#define NLK_MODE_SYS                0x0U
#define NLK_MODE_RPC                0x1U
#define NLK_MODE_BULK               0x4U

#define NLK_CAP_ID_CORE             0U
#define NLK_CAP_ID_FLASH            1U

#define NLK_CAP_CORE                0x0000U
#define NLK_CAP_FLASH               0x0001U

#define NLK_SUP_MODE_RPC            (1U << 0)
#define NLK_SUP_MODE_BULK           (1U << 3)

#define NLK_CORE_OP_GET_DEV_DESC    0x03U
#define NLK_CORE_OP_GET_CAP_LIST    0x04U
#define NLK_CORE_OP_GET_CAP_DESC    0x05U
#define NLK_CORE_OP_GET_TOPOLOGY    0x06U
#define NLK_CORE_OP_PING            0x07U
#define NLK_CORE_OP_RESET           0x08U
#define NLK_CORE_OP_REBOOT          0x09U

#define NLK_FLASH_OP_QUERY_PARTS    0x01U
#define NLK_FLASH_OP_PREPARE        0x02U
#define NLK_FLASH_OP_WRITE_CHUNK    0x03U
#define NLK_FLASH_OP_COMMIT         0x04U
#define NLK_FLASH_OP_ABORT          0x05U

#define NLK_FLASH_PART_F_WRITABLE       (1U << 0)
#define NLK_FLASH_PART_F_NEEDS_BL       (1U << 1)
#define NLK_FLASH_PART_F_REBOOT_AFTER   (1U << 2)

#define NLK_FLASH_MAX_PARTS         8U
#define NLK_FLASH_PART_NAME_MAX     16U

struct nlk_pkt_hdr {
    uint8_t  ver_flags;
    uint8_t  msg_type;
    uint16_t src_addr;
    uint16_t dst_addr;
    uint8_t  dst_cap;
    uint8_t  cmd_op;
    uint16_t seq_id;
    uint16_t payload_len;
} __attribute__((packed));

struct nlk_device_desc {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_class;
    uint8_t  hw_ver[2];
    uint8_t  fw_ver[4];
    uint8_t  uuid[12];
    char     name[16];
    uint16_t max_payload_size;
    uint8_t  cap_count;
    uint8_t  dev_flags;
    uint16_t parent_addr;
    uint8_t  attached_children;
    uint8_t  reserved;
} __attribute__((packed));

struct nlk_cap_item {
    uint8_t  cap_id;
    uint8_t  cap_ver;
    uint16_t cap_type;
    uint8_t  sup_modes;
    uint8_t  sub_class;
    uint16_t desc_size;
} __attribute__((packed));

struct nlk_topo_node_entry {
    uint16_t node_addr;
    uint16_t parent_addr;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_class;
    uint8_t  cap_count;
    uint8_t  port_id;
} __attribute__((packed));

struct nlk_core_reboot_req {
    uint32_t boot_flag;
} __attribute__((packed));

struct nlk_flash_part_desc {
    uint8_t  part_id;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t max_size;
    uint32_t erase_size;
    char     name[NLK_FLASH_PART_NAME_MAX];
} __attribute__((packed));

struct nlk_flash_query_resp {
    uint8_t  part_count;
    uint8_t  run_mode;
    uint16_t max_chunk_size;
    uint32_t running_version;
    struct nlk_flash_part_desc parts[NLK_FLASH_MAX_PARTS];
} __attribute__((packed));

struct nlk_flash_prepare_req {
    uint8_t  part_id;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t total_size;
    uint32_t expected_crc32;
    uint32_t version_code;
} __attribute__((packed));

struct nlk_flash_commit_req {
    uint8_t  part_id;
    uint8_t  reserved[3];
    uint32_t total_size;
    uint32_t expected_crc32;
} __attribute__((packed));

struct nlk_flash_chunk_hdr {
    uint32_t offset;
} __attribute__((packed));

struct nlk_flash_chunk_ack {
    uint32_t acked_offset;
    uint16_t rx_window_bitmap;
    int16_t  status;
} __attribute__((packed));

static inline bool nk60_validate_cortexm_vectors(uint32_t msp, uint32_t reset_pc,
                                                 uint32_t flash_start,
                                                 uint32_t flash_end)
{
    if (msp < (NK60_SRAM_BASE + 0x400UL) || msp > NK60_SRAM_END ||
        (msp & 0x3UL) != 0UL) {
        return false;
    }
    if ((reset_pc & 0x1UL) == 0UL) {
        return false;
    }
    if (reset_pc < (flash_start | 1UL) || reset_pc >= flash_end) {
        return false;
    }
    return true;
}

static inline bool nk60_validate_vector_table(const uint32_t *vec,
                                              uint32_t flash_start,
                                              uint32_t flash_end)
{
    static const uint8_t req_idx[5] = { 1U, 2U, 3U, 14U, 15U };
    uint32_t i;

    if (!nk60_validate_cortexm_vectors(vec[0], vec[1], flash_start, flash_end)) {
        return false;
    }
    for (i = 0UL; i < 5UL; i++) {
        uint32_t h = vec[req_idx[i]];
        if (h != 0UL &&
            ((h & 0x1UL) == 0UL || h < (flash_start | 1UL) || h >= flash_end)) {
            return false;
        }
    }
    return true;
}

#endif /* __NK60_BOOT_PROTO_H__ */
