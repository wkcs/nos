/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) Distributed Soft-Bus UAPI Protocol Definitions.
 * Invariant: All wire structures are packed, little-endian, and fixed-layout
 * across all architectures (ARM Cortex-M, ARM64, RISC-V, x86_64) and Host PCs.
 */

#ifndef __NOS_UAPI_NLK_H__
#define __NOS_UAPI_NLK_H__

#include <uapi/nos/types.h>

/* Protocol Version & Limits */
#define NLK_PROTO_MAJOR             1U
#define NLK_PROTO_MINOR             0U
#define NLK_PROTO_VER_1             (1U << 5)
#define NLK_PROTO_VER_MASK          0xE0U

#define NLK_SOF_BYTE0               0x55U
#define NLK_SOF_BYTE1               0xAAU
#define NLK_MS_READY_MAGIC          0xA5U
#define NLK_FW_IMG_MAGIC            0x20302030U

#define NLK_DEFAULT_TTL             8U
#define NLK_MAX_TIER                3U
#define NLK_MAX_PORTS_PER_NODE      15U

/* 16-bit Hierarchical Tree Node Address */
typedef uint16_t nlk_addr_t;

#define NLK_ADDR_HOST               ((nlk_addr_t)0x0000U)
#define NLK_ADDR_PRIMARY            ((nlk_addr_t)0x0001U)
#define NLK_ADDR_LOCAL              ((nlk_addr_t)0xFFFEU)
#define NLK_ADDR_UNASSIGNED         ((nlk_addr_t)0xFFFEU)
#define NLK_ADDR_BROADCAST          ((nlk_addr_t)0xFFFFU)

/* Routing action results */
#define NLK_ROUTE_DROP              (-1)
#define NLK_ROUTE_LOCAL             0
#define NLK_ROUTE_UPSTREAM          16
#define NLK_ROUTE_BROADCAST         17

/* Compute tier (0..3) of a hierarchical node address (0x0001 -> 0, 0x0011 -> 1, 0x0111 -> 2, 0x1111 -> 3) */
static inline uint8_t nlk_addr_get_tier(nlk_addr_t addr)
{
    if (addr == NLK_ADDR_HOST || addr >= NLK_ADDR_LOCAL) {
        return 0U;
    }
    if ((addr & 0xF000U) != 0U) {
        return 3U;
    }
    if ((addr & 0x0F00U) != 0U) {
        return 2U;
    }
    if ((addr & 0x00F0U) != 0U) {
        return 1U;
    }
    return 0U;
}

/* Construct child hierarchical address given parent address and downstream port (1..15) */
static inline nlk_addr_t nlk_addr_make_child(nlk_addr_t parent_addr, uint8_t child_port)
{
    uint8_t tier;

    if (child_port == 0U || child_port > NLK_MAX_PORTS_PER_NODE) {
        return NLK_ADDR_UNASSIGNED;
    }
    if (parent_addr == NLK_ADDR_HOST) {
        return NLK_ADDR_PRIMARY;
    }
    tier = nlk_addr_get_tier(parent_addr);
    if (tier >= NLK_MAX_TIER) {
        return NLK_ADDR_UNASSIGNED;
    }
    return (nlk_addr_t)(parent_addr | ((uint16_t)(child_port & 0x0FU) << ((tier + 1U) * 4U)));
}

/* O(1) zero-table hierarchical route lookup */
static inline int nlk_route_lookup(nlk_addr_t my_addr, nlk_addr_t dst_addr)
{
    uint8_t my_tier;
    uint16_t my_mask;
    uint8_t child_port;

    if (dst_addr == my_addr || dst_addr == NLK_ADDR_LOCAL) {
        return NLK_ROUTE_LOCAL;
    }
    if (dst_addr == NLK_ADDR_BROADCAST) {
        return NLK_ROUTE_BROADCAST;
    }
    if (my_addr == NLK_ADDR_HOST) {
        /* Host routes all device traffic through its primary root link (port 1) */
        if ((dst_addr & 0x000FU) == NLK_ADDR_PRIMARY) {
            return 1;
        }
        return NLK_ROUTE_DROP;
    }

    my_tier = nlk_addr_get_tier(my_addr);
    my_mask = (uint16_t)((1UL << ((my_tier + 1U) * 4U)) - 1UL);

    if (dst_addr == NLK_ADDR_HOST || (dst_addr & my_mask) != my_addr) {
        return NLK_ROUTE_UPSTREAM;
    }
    if (my_tier >= NLK_MAX_TIER) {
        return NLK_ROUTE_DROP;
    }
    child_port = (uint8_t)((dst_addr >> ((my_tier + 1U) * 4U)) & 0x0FU);
    if (child_port == 0U) {
        return NLK_ROUTE_LOCAL;
    }
    return (int)child_port;
}

/* Packet Header Flags (ver_flags[4:0]) */
#define NLK_FLAG_NONE               0x00U
#define NLK_FLAG_ACK_REQ            (1U << 0)
#define NLK_FLAG_IS_ACK             (1U << 1)
#define NLK_FLAG_IS_ERR             (1U << 2)
#define NLK_FLAG_FRAG               (1U << 3)
#define NLK_FLAG_MORE_FRAG          (1U << 4)
#define NLK_FLAG_MASK               0x1FU

/* Communication Modes (msg_type[7:4]) */
enum nlk_comm_mode {
    NLK_MODE_SYS    = 0x0U,
    NLK_MODE_RPC    = 0x1U,
    NLK_MODE_EVENT  = 0x2U,
    NLK_MODE_STREAM = 0x3U,
    NLK_MODE_BULK   = 0x4U,
};

#define NLK_VER_FLAGS_MAKE(flags)   ((uint8_t)(NLK_PROTO_VER_1 | ((flags) & NLK_FLAG_MASK)))
#define NLK_PKT_FLAGS(vf)           ((uint8_t)((vf) & NLK_FLAG_MASK))
#define NLK_PKT_VER(vf)             ((uint8_t)((vf) & NLK_PROTO_VER_MASK))

#define NLK_MSG_TYPE_MAKE(mode, ttl) \
    ((uint8_t)((((uint8_t)(mode) & 0x0FU) << 4) | ((uint8_t)(ttl) & 0x0FU)))
#define NLK_MSG_MODE(mt)            ((uint8_t)(((mt) >> 4) & 0x0FU))
#define NLK_MSG_TTL(mt)             ((uint8_t)((mt) & 0x0FU))

/* Well-known Capability Instance IDs on a device */
#define NLK_CAP_ID_CORE             0U
#define NLK_CAP_ID_BROADCAST        255U

/* L3/L4 Unified 12-Byte Packet Header */
struct nlk_pkt_hdr {
    uint8_t  ver_flags;     /* [7:5] proto version, [4:0] NLK_FLAG_* */
    uint8_t  msg_type;      /* [7:4] nlk_comm_mode, [3:0] hop TTL */
    uint16_t src_addr;      /* Source node hierarchical address */
    uint16_t dst_addr;      /* Destination node hierarchical address */
    uint8_t  dst_cap;       /* Target capability instance ID on dst_addr */
    uint8_t  cmd_op;        /* Capability-specific opcode or event ID */
    uint16_t seq_id;        /* Transaction sequence / stream seq / bulk chunk ID */
    uint16_t payload_len;   /* Byte length of payload following this header */
} __attribute__((packed));

/* Optional 4-Byte L3 Fragmentation Extension Header (present when NLK_FLAG_FRAG is set) */
struct nlk_frag_hdr {
    uint16_t total_len;     /* Unfragmented total payload length */
    uint16_t frag_offset;   /* Byte offset of this fragment */
} __attribute__((packed));

/* L2 Master-Slave (I2C/SPI) 4-Byte Status Poll Header */
struct nlk_ms_poll_hdr {
    uint8_t  magic;         /* NLK_MS_READY_MAGIC (0xA5) */
    uint8_t  status_flags;  /* BIT0: TX_PENDING, BIT1: BUSY, BIT2: ERR */
    uint16_t pending_len;   /* Pending NLK packet length (0 if idle) */
} __attribute__((packed));

/* L2 CAN 29-Bit Extended Identifier Mapping */
#define NLK_CAN_SAR_SINGLE          0U
#define NLK_CAN_SAR_FIRST           1U
#define NLK_CAN_SAR_CONSEC          2U
#define NLK_CAN_SAR_LAST            3U

#define NLK_CAN_ID_MAKE(prio, dst_slot, src_slot, dst_cap, sar, seq) \
    ((((uint32_t)(prio) & 0x07UL) << 26) |      \
     (((uint32_t)(dst_slot) & 0x1FUL) << 21) |  \
     (((uint32_t)(src_slot) & 0x1FUL) << 16) |  \
     (((uint32_t)(dst_cap) & 0xFFUL) << 8) |    \
     (((uint32_t)(sar) & 0x03UL) << 6) |        \
     ((uint32_t)(seq) & 0x3FUL))

#define NLK_CAN_ID_PRIO(id)         ((uint8_t)(((id) >> 26) & 0x07U))
#define NLK_CAN_ID_DST_SLOT(id)     ((uint8_t)(((id) >> 21) & 0x1FU))
#define NLK_CAN_ID_SRC_SLOT(id)     ((uint8_t)(((id) >> 16) & 0x1FU))
#define NLK_CAN_ID_DST_CAP(id)      ((uint8_t)(((id) >> 8) & 0xFFU))
#define NLK_CAN_ID_SAR(id)          ((uint8_t)(((id) >> 6) & 0x03U))
#define NLK_CAN_ID_SEQ(id)          ((uint8_t)((id) & 0x3FU))

/* Standard Device Classes */
enum nlk_device_class {
    NLK_DEV_CLASS_GENERIC   = 0x0000U,
    NLK_DEV_CLASS_HOST      = 0x0001U,
    NLK_DEV_CLASS_KEYBOARD  = 0x0010U,
    NLK_DEV_CLASS_KEYPAD    = 0x0011U,
    NLK_DEV_CLASS_ENCODER   = 0x0012U,
    NLK_DEV_CLASS_DISPLAY   = 0x0020U,
    NLK_DEV_CLASS_WIRELESS  = 0x0030U,
    NLK_DEV_CLASS_HUB       = 0x0040U,
};

/* Device Capability Flags (dev_flags) */
#define NLK_DEV_FLAG_BRIDGE         (1U << 0)
#define NLK_DEV_FLAG_BATTERY        (1U << 1)
#define NLK_DEV_FLAG_SLEEPABLE      (1U << 2)

/* Globally Standardized Capability Type IDs (cap_type) */
enum nlk_cap_type {
    /* System & DevOps Family (0x0000 - 0x00FF) */
    NLK_CAP_CORE        = 0x0000U,
    NLK_CAP_FLASH       = 0x0001U,
    NLK_CAP_LOG         = 0x0002U,
    NLK_CAP_SHELL       = 0x0003U,
    NLK_CAP_VFS         = 0x0004U,
    NLK_CAP_SYSMON      = 0x0005U,

    /* Human Input Family (0x0100 - 0x01FF) */
    NLK_CAP_KEYBOARD    = 0x0100U,
    NLK_CAP_ENCODER     = 0x0101U,
    NLK_CAP_KEYPAD      = 0x0102U,
    NLK_CAP_POINTER     = 0x0103U,

    /* Visual & Feedback Family (0x0200 - 0x02FF) */
    NLK_CAP_DISPLAY     = 0x0200U,
    NLK_CAP_AUDIO       = 0x0202U,

    /* Wireless & Connectivity Family (0x0300 - 0x03FF) */
    NLK_CAP_WIRELESS    = 0x0300U,

    /* Vendor Specific (0x8000 - 0xFFFF) */
    NLK_CAP_VENDOR_BASE = 0x8000U,
};

/* Supported Mode Bitmask for struct nlk_cap_item::sup_modes */
#define NLK_SUP_MODE_RPC            (1U << 0)
#define NLK_SUP_MODE_EVENT          (1U << 1)
#define NLK_SUP_MODE_STREAM         (1U << 2)
#define NLK_SUP_MODE_BULK           (1U << 3)

/* =========================================================================
 * Cap 0 (0x0000): NLK_CAP_CORE — Discovery, Enumeration & Topology
 * ========================================================================= */
enum nlk_core_opcode {
    NLK_CORE_OP_HELLO           = 0x01U,
    NLK_CORE_OP_ASSIGN_ADDR     = 0x02U,
    NLK_CORE_OP_GET_DEV_DESC    = 0x03U,
    NLK_CORE_OP_GET_CAP_LIST    = 0x04U,
    NLK_CORE_OP_GET_CAP_DESC    = 0x05U,
    NLK_CORE_OP_GET_TOPOLOGY    = 0x06U,
    NLK_CORE_OP_PING            = 0x07U,
    NLK_CORE_OP_RESET           = 0x08U,
    NLK_CORE_OP_REBOOT          = 0x09U,
    NLK_CORE_EVT_TOPO_CHANGE    = 0x20U,
};

struct nlk_core_reboot_req {
    uint32_t boot_flag;     /* 对应内核 enum boot_flag (BOOT_NORMAL / BOOT_BOOTLOADER / BOOT_RECOVERY) */
} __attribute__((packed));

struct nlk_hello_msg {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_class;
    uint16_t max_payload_size;
    uint8_t  uuid[12];
    uint8_t  cap_count;
    uint8_t  dev_flags;
    uint16_t cur_addr;
} __attribute__((packed));

struct nlk_assign_addr_msg {
    uint16_t assigned_addr;
    uint16_t parent_addr;
    uint16_t path_mtu;
    uint16_t heartbeat_ms;
    uint8_t  uuid[12];
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

#define NLK_TOPO_EVT_ATTACHED       1U
#define NLK_TOPO_EVT_DETACHED       2U

struct nlk_topo_node_entry {
    uint16_t node_addr;
    uint16_t parent_addr;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_class;
    uint8_t  cap_count;
    uint8_t  port_id;
} __attribute__((packed));

struct nlk_topo_event_msg {
    uint8_t                    event_type; /* NLK_TOPO_EVT_ATTACHED / DETACHED */
    uint8_t                    hop_count;
    uint16_t                   topo_seq;
    struct nlk_topo_event_info {
        uint16_t node_addr;
        uint16_t parent_addr;
        uint16_t vendor_id;
        uint16_t product_id;
        uint16_t device_class;
        uint8_t  cap_count;
        uint8_t  port_id;
    } __attribute__((packed)) node;
} __attribute__((packed));

/* =========================================================================
 * Cap 0x0001: NLK_CAP_FLASH — 通用分区注册与固件刷写能力
 * ========================================================================= */
enum nlk_flash_opcode {
    NLK_FLASH_OP_QUERY_PARTS    = 0x01U,
    NLK_FLASH_OP_PREPARE        = 0x02U,
    NLK_FLASH_OP_WRITE_CHUNK    = 0x03U,
    NLK_FLASH_OP_COMMIT         = 0x04U,
    NLK_FLASH_OP_ABORT          = 0x05U,
};

/* 分区属性标志位 */
#define NLK_FLASH_PART_F_WRITABLE       (1U << 0) /* 允许刷写 */
#define NLK_FLASH_PART_F_NEEDS_BL       (1U << 1) /* 当前处于运行态 XIP，需先重启到 Bootloader 刷写 */
#define NLK_FLASH_PART_F_REBOOT_AFTER   (1U << 2) /* 刷写提交后需重启生效 */

#define NLK_FLASH_MAX_PARTS             8U
#define NLK_FLASH_PART_NAME_MAX         16U

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
    uint8_t  run_mode;      /* 0 = BOOT_NORMAL, 1 = BOOT_BOOTLOADER */
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

/* =========================================================================
 * Cap 0x0002: NLK_CAP_LOG — Kernel printk Log Stream & Control
 * ========================================================================= */
enum nlk_log_opcode {
    NLK_LOG_OP_SUBSCRIBE        = 0x01U,
    NLK_LOG_OP_SET_LEVEL        = 0x02U,
    NLK_LOG_OP_GET_STATS        = 0x03U,
    NLK_LOG_STREAM_RECORD       = 0x10U,
};

struct nlk_log_sub_req {
    uint8_t  enable;
    uint8_t  min_level;
    uint16_t flush_interval_ms;
} __attribute__((packed));

struct nlk_log_stats_resp {
    uint32_t dropped_records;
    uint32_t truncated_records;
    uint32_t high_water;
    uint8_t  cur_level;
    uint8_t  enabled;
    uint16_t subscriber_addr;
} __attribute__((packed));

struct nlk_log_wire_hdr {
    uint32_t timestamp_ms;
    uint16_t length;
    uint8_t  level;
    uint8_t  flags;
} __attribute__((packed));

/* =========================================================================
 * Cap 0x0003: NLK_CAP_SHELL — Remote nShell / Virtual TTY Stream
 * ========================================================================= */
enum nlk_shell_opcode {
    NLK_SHELL_OP_OPEN           = 0x01U,
    NLK_SHELL_OP_CLOSE          = 0x02U,
    NLK_SHELL_OP_SIGNAL         = 0x03U,
    NLK_SHELL_OP_FLOW_CRED      = 0x04U,
    NLK_SHELL_OP_INPUT          = 0x05U,
    NLK_SHELL_OP_HEARTBEAT      = 0x06U,
    NLK_SHELL_OP_EXEC           = 0x07U,
    NLK_SHELL_STREAM_STDIN      = 0x10U,
    NLK_SHELL_STREAM_STDOUT     = 0x11U,
    NLK_SHELL_STREAM_STDOUT_V2  = 0x12U,
    NLK_SHELL_STREAM_EXIT       = 0x13U,
};

#define NLK_SHELL_SIG_INT           2U
#define NLK_SHELL_SIG_QUIT          3U

struct nlk_shell_open_msg {
    uint16_t cols;
    uint16_t rows;
    uint16_t initial_credit;
    uint16_t flags;
} __attribute__((packed));

/* Optional reliable session extension. All integers use NLK little endian. */
#define NLK_SHELL_OPEN_RELIABLE         (1U << 0)
#define NLK_SHELL_OPEN_NONINTERACTIVE   (1U << 1)
#define NLK_SHELL_INPUT_CHUNK_MAX       64U
struct nlk_shell_open_v2_msg {
    struct nlk_shell_open_msg open;
    uint64_t session_id;
} __attribute__((packed));

struct nlk_shell_input_hdr {
    uint64_t session_id;
    uint32_t offset;
} __attribute__((packed));

struct nlk_shell_exit_msg {
    int32_t  exit_code;
} __attribute__((packed));

/* =========================================================================
 * Cap 0x0004: NLK_CAP_VFS — 远端虚拟文件系统协议（文件读写、目录枚举与属性管理）
 * ========================================================================= */
enum nlk_vfs_opcode {
    NLK_VFS_OP_OPEN             = 0x01U,
    NLK_VFS_OP_CLOSE            = 0x02U,
    NLK_VFS_OP_READ             = 0x03U,
    NLK_VFS_OP_WRITE            = 0x04U,
    NLK_VFS_OP_STAT             = 0x05U,
    NLK_VFS_OP_READDIR          = 0x06U,
    NLK_VFS_OP_MKDIR            = 0x07U,
    NLK_VFS_OP_REMOVE           = 0x08U,
    NLK_VFS_OP_RENAME           = 0x09U,
    NLK_VFS_OP_STATFS           = 0x0AU,
};

/* 跨平台文件打开标志位 */
#define NLK_VFS_O_RDONLY            0x0001U
#define NLK_VFS_O_WRONLY            0x0002U
#define NLK_VFS_O_RDWR              0x0003U
#define NLK_VFS_O_ACCMODE           0x0003U
#define NLK_VFS_O_CREAT             0x0100U
#define NLK_VFS_O_TRUNC             0x0200U
#define NLK_VFS_O_APPEND            0x0400U

/* 跨平台目录项类型 */
#define NLK_VFS_DT_UNKNOWN          0U
#define NLK_VFS_DT_REG              1U
#define NLK_VFS_DT_DIR              2U
#define NLK_VFS_DT_CHR              3U
#define NLK_VFS_DT_BLK              4U

#define NLK_VFS_PATH_MAX            128U
#define NLK_VFS_NAME_MAX            64U
#define NLK_VFS_MAX_HANDLES         8U

struct nlk_vfs_open_req {
    uint16_t flags;         /* NLK_VFS_O_* */
    uint16_t mode;          /* 权限位，如 0644 */
    char     path[NLK_VFS_PATH_MAX];
} __attribute__((packed));

struct nlk_vfs_open_resp {
    int32_t  handle;        /* 会话文件句柄 (0..NLK_VFS_MAX_HANDLES-1) */
    uint32_t reserved;
    uint64_t file_size;     /* 当前文件字节大小 */
} __attribute__((packed));

struct nlk_vfs_close_req {
    int32_t  handle;
} __attribute__((packed));

struct nlk_vfs_read_req {
    int32_t  handle;
    uint32_t offset;
    uint16_t count;
    uint16_t reserved;
} __attribute__((packed));

struct nlk_vfs_read_resp_hdr {
    uint16_t bytes_read;
    uint8_t  eof;
    uint8_t  reserved;
    /* 紧随其后为 uint8_t data[bytes_read] */
} __attribute__((packed));

struct nlk_vfs_write_req_hdr {
    int32_t  handle;
    uint32_t offset;
    uint16_t count;
    uint16_t reserved;
    /* 紧随其后为 uint8_t data[count] */
} __attribute__((packed));

struct nlk_vfs_write_resp {
    uint16_t bytes_written;
    uint16_t reserved;
} __attribute__((packed));

struct nlk_vfs_path_req {
    uint16_t mode;          /* 用于 MKDIR 等，其余操作置 0 */
    uint16_t reserved;
    char     path[NLK_VFS_PATH_MAX];
} __attribute__((packed));

struct nlk_vfs_rename_req {
    char     old_path[NLK_VFS_PATH_MAX];
    char     new_path[NLK_VFS_PATH_MAX];
} __attribute__((packed));

struct nlk_vfs_stat_resp {
    uint64_t size;
    uint64_t mtime;
    uint32_t mode;
    uint8_t  type;          /* NLK_VFS_DT_* */
    uint8_t  reserved[3];
} __attribute__((packed));

struct nlk_vfs_readdir_req {
    uint16_t start_index;
    uint16_t max_entries;
    char     path[NLK_VFS_PATH_MAX];
} __attribute__((packed));

struct nlk_vfs_dirent_wire {
    uint64_t size;
    uint32_t mode;
    uint8_t  type;          /* NLK_VFS_DT_* */
    uint8_t  name_len;
    uint16_t rec_len;       /* 包含本头与对齐后文件名的总字节数 */
    /* 紧随其后为 char name[name_len + 1] 并按 4 字节对齐 */
} __attribute__((packed));

struct nlk_vfs_readdir_resp_hdr {
    uint16_t next_index;
    uint8_t  eof;
    uint8_t  entry_count;
    /* 紧随其后为 entry_count 个变长 struct nlk_vfs_dirent_wire */
} __attribute__((packed));

struct nlk_vfs_statfs_resp {
    uint32_t bsize;
    uint32_t blocks;
    uint32_t bfree;
    uint32_t bavail;
    uint32_t namelen;
} __attribute__((packed));

/* =========================================================================
 * Cap 0x0100 / 0x0102: NLK_CAP_KEYBOARD & NLK_CAP_KEYPAD
 * Supports diverse keyboard form factors, switch types, NKRO, analog/RT,
 * and integrated keyboard lighting (per-key RGB, underglow, indicators, sync).
 * ========================================================================= */
enum nlk_kb_opcode {
    NLK_KB_OP_GET_DESC              = 0x01U,
    NLK_KB_OP_GET_KEYMAP            = 0x02U,
    NLK_KB_OP_SET_KEYMAP            = 0x03U,
    NLK_KB_OP_GET_SET_PARAM         = 0x04U,
    NLK_KB_OP_GET_LIGHT_CFG         = 0x05U,
    NLK_KB_OP_SET_LIGHT_CFG         = 0x06U,
    NLK_KB_OP_SET_INDICATORS        = 0x07U,
    NLK_KB_OP_GET_ANALOG_CFG        = 0x08U,
    NLK_KB_OP_SET_ANALOG_CFG        = 0x09U,
    NLK_KB_OP_GET_NKRO_REPORT       = 0x0AU,
    NLK_KB_OP_GET_LAYOUT_LIST       = 0x0BU,
    NLK_KB_OP_SET_LAYOUT            = 0x0CU,
    NLK_KB_OP_GET_LIGHT_ZONES       = 0x0DU,
    NLK_KB_OP_GET_ZONE_LIGHT_CFG    = 0x0EU,
    NLK_KB_OP_SET_ZONE_LIGHT_CFG    = 0x0FU,
    NLK_KB_STREAM_LIGHT_FRAME       = 0x10U,
    NLK_KB_OP_GET_LAYERS            = 0x11U,
    NLK_KB_OP_SET_ACTIVE_LAYER      = 0x12U,
    NLK_KB_OP_GET_MACRO_LIST        = 0x13U,
    NLK_KB_OP_GET_MACRO             = 0x14U,
    NLK_KB_OP_SET_MACRO             = 0x15U,
    NLK_KB_OP_RECORD_MACRO          = 0x16U,
    NLK_KB_OP_GET_ACTIONS           = 0x17U,
    NLK_KB_OP_GET_PARAMS            = 0x18U,
    NLK_KB_OP_SET_PARAMS            = 0x19U,
    NLK_KB_OP_RESET_PARAMS          = 0x1AU,
    NLK_KB_OP_GET_PROFILE_INFO      = 0x1BU,
    NLK_KB_OP_SAVE_PROFILE          = 0x1CU,
    NLK_KB_OP_LOAD_PROFILE          = 0x1DU,
    NLK_KB_OP_BIND_ACTION           = 0x24U,
    NLK_KB_OP_UNBIND_ACTION         = 0x25U,
    NLK_KB_OP_GET_KEY_ACTIONS       = 0x26U,
    NLK_KB_OP_GET_LIGHT_EFFECTS     = 0x27U,
    NLK_KB_OP_GET_LIGHT_EFFECTS_PAGE = 0x28U,
    NLK_KB_OP_CONFIG_FILES_INFO     = 0x29U,
    NLK_KB_OP_CONFIG_FILES_LIST     = 0x2AU,
    NLK_KB_OP_CONFIG_FILES_SAVE     = 0x2BU,
    NLK_KB_OP_CONFIG_FILES_LOAD     = 0x2CU,
    NLK_KB_OP_CONFIG_FILES_DELETE   = 0x2DU,
    NLK_KB_OP_MACRO_CONTROL         = 0x2EU,
    NLK_KB_OP_MACRO_STATUS          = 0x2FU,
    NLK_KB_EVT_SWITCH_DELTA         = 0x20U,
    NLK_KB_EVT_HID_REPORT           = 0x21U,
    NLK_KB_EVT_LIGHT_SYNC_BEACON    = 0x22U,
    NLK_KB_EVT_NKRO_REPORT          = 0x23U,
};

/* Keyboard Form Factors / Morphologies */
enum nlk_kb_form_factor {
    NLK_KB_FORM_ANSI_100    = 0x01U, /* Full-size 100% (104/108 keys) */
    NLK_KB_FORM_96          = 0x02U, /* 96% / 98% / 1800 compact */
    NLK_KB_FORM_TKL_80      = 0x03U, /* 80% Tenkeyless (87/88 keys) */
    NLK_KB_FORM_75          = 0x04U, /* 75% compact (82-84 keys) */
    NLK_KB_FORM_65          = 0x05U, /* 65% compact (67-68 keys) */
    NLK_KB_FORM_60          = 0x06U, /* 60% compact (61-64 keys) */
    NLK_KB_FORM_40          = 0x07U, /* 40% mini (40-48 keys) */
    NLK_KB_FORM_SPLIT       = 0x08U, /* Split ergonomic (Left + Right halves) */
    NLK_KB_FORM_ORTHO       = 0x09U, /* Ortholinear / Column-staggered */
    NLK_KB_FORM_ALICE       = 0x0AU, /* Alice / Arisu ergonomic */
    NLK_KB_FORM_NUMPAD      = 0x0BU, /* Standalone / Cascaded Numpad */
    NLK_KB_FORM_MACROPAD    = 0x0CU, /* Macropad / One-handed keypad */
    NLK_KB_FORM_CUSTOM      = 0xFFU, /* Custom modular matrix */
};

/* Keyboard Switch Technologies */
enum nlk_kb_switch_type {
    NLK_KB_SW_MECH          = 0x00U, /* Mechanical contact switch */
    NLK_KB_SW_HALL_ANALOG   = 0x01U, /* Hall-effect magnetic analog switch */
    NLK_KB_SW_OPTICAL       = 0x02U, /* Optical switch */
    NLK_KB_SW_EC            = 0x03U, /* Electrostatic capacitive switch */
    NLK_KB_SW_LOW_PROFILE   = 0x04U, /* Low-profile switch */
    NLK_KB_SW_SCISSOR       = 0x05U, /* Scissor / membrane switch */
};

/* Keyboard Capability Feature Flags (kb_flags) */
#define NLK_KB_FLAG_6KRO                (1U << 0)
#define NLK_KB_FLAG_NKRO                (1U << 1)
#define NLK_KB_FLAG_MEDIA_KEYS          (1U << 2)
#define NLK_KB_FLAG_SPLIT_SLAVE         (1U << 3)
#define NLK_KB_FLAG_MODULAR_HUB         (1U << 4)
#define NLK_KB_FLAG_ANALOG_RT           (1U << 5)
#define NLK_KB_FLAG_LIGHT_MONO          (1U << 6)
#define NLK_KB_FLAG_LIGHT_RGB_KEY       (1U << 7)
#define NLK_KB_FLAG_LIGHT_UNDERGLOW     (1U << 8)
#define NLK_KB_FLAG_LIGHT_INDICATORS    (1U << 9)

/* Keyboard Status Indicator LED Bitmask */
#define NLK_KB_IND_NUM_LOCK             (1U << 0)
#define NLK_KB_IND_CAPS_LOCK            (1U << 1)
#define NLK_KB_IND_SCROLL_LOCK          (1U << 2)
#define NLK_KB_IND_COMPOSE              (1U << 3)
#define NLK_KB_IND_KANA                 (1U << 4)

#define NLK_KB_LIGHT_ZONE_KEYS          (1U << 0)
#define NLK_KB_LIGHT_ZONE_UNDERGLOW     (1U << 1)
#define NLK_KB_LIGHT_ZONE_INDICATORS    (1U << 2)
#define NLK_KB_LIGHT_ZONE_ALL           0x07U

struct nlk_kb_desc {
    uint8_t  form_factor;       /* enum nlk_kb_form_factor */
    uint8_t  switch_type;       /* enum nlk_kb_switch_type */
    uint16_t kb_flags;          /* NLK_KB_FLAG_* bitmask */
    uint8_t  matrix_rows;
    uint8_t  matrix_cols;
    uint8_t  key_count;
    uint8_t  layer_count;
    uint8_t  debounce_ms;
    uint8_t  led_matrix_rows;
    uint8_t  led_matrix_cols;
    uint8_t  underglow_leds;
    uint16_t led_count;
    uint16_t poll_rate_hz;
    uint32_t ext_module_mask;
} __attribute__((packed));

struct nlk_kb_keymap_hdr {
    uint8_t  module_id;
    uint8_t  layer;
    uint8_t  start_index;
    uint8_t  count;
} __attribute__((packed));

struct nlk_kb_keymap_entry {
    uint8_t  key_code;
    uint8_t  action;
} __attribute__((packed));

struct nlk_kb_switch_item {
    uint8_t  key_index;
    uint8_t  state;     /* 0 = KEY_UP, 1 = KEY_DOWN */
    uint8_t  key_code;
    uint8_t  action;
} __attribute__((packed));

struct nlk_kb_switch_event_hdr {
    uint8_t  module_id;
    uint8_t  active_layer;
    uint8_t  event_count;
    uint8_t  reserved;
} __attribute__((packed));

struct nlk_kb_hid_report {
    uint8_t  report_id;
    uint8_t  modifiers;
    uint8_t  reserved;
    uint8_t  keys[6];
} __attribute__((packed));

struct nlk_kb_nkro_report {
    uint8_t  report_id;
    uint8_t  modifiers;
    uint8_t  bitmap[16];    /* 128-bit HID keycode bitmap (0x00..0x7F) */
} __attribute__((packed));

struct nlk_kb_analog_cfg {
    uint8_t  actuation_01mm;    /* Actuation point in 0.1mm (e.g. 20 = 2.0mm) */
    uint8_t  rapid_trigger_en;  /* 0 = off, 1 = continuous RT, 2 = full-stroke RT */
    uint8_t  rt_press_01mm;     /* RT press sensitivity in 0.1mm */
    uint8_t  rt_release_01mm;   /* RT release sensitivity in 0.1mm */
    uint8_t  bottom_deadzone;   /* Bottom deadzone in 0.1mm */
    uint8_t  top_deadzone;      /* Top deadzone in 0.1mm */
    uint16_t reserved;
} __attribute__((packed));

struct nlk_kb_light_cfg {
    uint8_t  enable;
    uint8_t  zone_mask;         /* NLK_KB_LIGHT_ZONE_* */
    uint8_t  effect_id;         /* Device-local registered effect ID */
    uint8_t  brightness;        /* 0..255 */
    uint8_t  speed;             /* 0..255 */
    uint8_t  color_r;
    uint8_t  color_g;
    uint8_t  color_b;
} __attribute__((packed));

struct nlk_kb_light_frame_hdr {
    uint16_t start_led;
    uint16_t led_count;
} __attribute__((packed));

#define NLK_KB_LIGHT_EFFECTS_PER_PAGE   6U
#define NLK_KB_LIGHT_EFFECT_NAME_MAX    16U
#define NLK_KB_LIGHT_EFFECT_DESC_MAX    20U

struct nlk_kb_light_sync_beacon {
    uint32_t master_frame_tick;
    uint8_t  effect_id;
    uint8_t  speed;
    uint8_t  brightness;
    uint8_t  hue_offset;
} __attribute__((packed));

/* Optional suffix lets peers translate IDs through their own registries. */
struct nlk_kb_named_light_sync_beacon {
    struct nlk_kb_light_sync_beacon beacon;
    char effect_name[NLK_KB_LIGHT_EFFECT_NAME_MAX];
} __attribute__((packed));

/* =========================================================================
 * Multi-Layout, Multi-Zone Backlight, Layers, Macros, Actions & Parameters
 * ========================================================================= */

/* Predefined Keyboard Layout Identifiers */
#define NLK_KB_LAYOUT_STD_60_ANSI       0x01U /* 60% Standard ANSI (61 keys) */
#define NLK_KB_LAYOUT_STD_60_ISO        0x02U /* 60% Standard ISO (62 keys) */
#define NLK_KB_LAYOUT_ARROW_60          0x03U /* 60% with dedicated Arrow cluster (64 keys) */
#define NLK_KB_LAYOUT_HHKB_60           0x04U /* 60% HHKB ergonomic layout (60 keys) */
#define NLK_KB_LAYOUT_STD_65            0x05U /* 65% Compact (67-68 keys) */
#define NLK_KB_LAYOUT_STD_75            0x06U /* 75% Compact (82-84 keys) */
#define NLK_KB_LAYOUT_TKL_87_ANSI       0x07U /* 87-key Tenkeyless ANSI */
#define NLK_KB_LAYOUT_TKL_88_ISO        0x08U /* 88-key Tenkeyless ISO */
#define NLK_KB_LAYOUT_FULL_104_ANSI     0x09U /* 104-key Full ANSI */
#define NLK_KB_LAYOUT_FULL_108_ANSI     0x0AU /* 108-key Full ANSI with Media */
#define NLK_KB_LAYOUT_ALICE_ERGO        0x0BU /* Alice / Arisu Ergonomic */
#define NLK_KB_LAYOUT_SPLIT_68          0x0CU /* Split Ergonomic 68-key */
#define NLK_KB_LAYOUT_NUMPAD_21         0x0DU /* Standalone Numpad 21-key */
#define NLK_KB_LAYOUT_CUSTOM            0xFFU /* Custom Matrix Layout */

#define NLK_KB_MAX_LAYOUTS              4U
#define NLK_KB_LAYOUT_NAME_MAX          16U

struct nlk_kb_layout_info {
    uint8_t  layout_id;            /* Predefined NLK_KB_LAYOUT_* or custom */
    uint8_t  key_count;            /* Key count */
    uint8_t  matrix_rows;          /* Physical matrix rows */
    uint8_t  matrix_cols;          /* Physical matrix cols */
    char     name[NLK_KB_LAYOUT_NAME_MAX]; /* e.g. "ANSI-61", "Arrow-64" */
} __attribute__((packed));

struct nlk_kb_layout_list_resp {
    uint8_t  layout_count;         /* Number of registered layouts */
    uint8_t  active_layout_id;     /* Currently active layout ID */
    uint16_t reserved;
    struct nlk_kb_layout_info layouts[NLK_KB_MAX_LAYOUTS];
} __attribute__((packed));

struct nlk_kb_set_layout_req {
    uint8_t  layout_id;
    uint8_t  reserved[3];
} __attribute__((packed));

/* Multi-Zone Backlight Layouts & Profiles */
enum nlk_kb_light_type {
    NLK_KB_LIGHT_TYPE_NONE          = 0U,
    NLK_KB_LIGHT_TYPE_MONO          = 1U, /* Single-color backlight */
    NLK_KB_LIGHT_TYPE_RGB_MATRIX    = 2U, /* Per-key RGB Matrix */
    NLK_KB_LIGHT_TYPE_UNDERGLOW     = 3U, /* Peripheral Underglow / Ambient RGB */
    NLK_KB_LIGHT_TYPE_INDICATOR     = 4U, /* Status Indicator LEDs */
    NLK_KB_LIGHT_TYPE_LOGO          = 5U, /* Badge / Logo LED */
};

#define NLK_KB_MAX_LIGHT_ZONES          4U
#define NLK_KB_ZONE_NAME_MAX            16U

struct nlk_kb_light_zone_desc {
    uint8_t  zone_id;              /* Zone ID (0..MAX-1) */
    uint8_t  type;                 /* enum nlk_kb_light_type */
    uint16_t led_start;            /* First LED index in string/matrix */
    uint16_t led_count;            /* Number of LEDs in zone */
    uint16_t supported_effects;    /* Supported effect bitmask (1 << effect_id) */
    char     name[NLK_KB_ZONE_NAME_MAX]; /* e.g. "Key-RGB", "Underglow" */
} __attribute__((packed));

struct nlk_kb_light_zones_resp {
    uint8_t  zone_count;           /* Number of registered light zones */
    uint8_t  global_enable;        /* Global backlight master switch */
    uint16_t reserved;
    struct nlk_kb_light_zone_desc zones[NLK_KB_MAX_LIGHT_ZONES];
} __attribute__((packed));

struct nlk_kb_zone_light_cfg {
    uint8_t  zone_id;              /* Target zone ID */
    uint8_t  enable;               /* Zone enable switch */
    uint8_t  effect_id;            /* Device-local registered effect ID */
    uint8_t  brightness;           /* 0..255 */
    uint8_t  speed;                /* 0..255 */
    uint8_t  color_r;              /* RGB Red */
    uint8_t  color_g;              /* RGB Green */
    uint8_t  color_b;              /* RGB Blue */
} __attribute__((packed));

/* Multi-Layer Configuration */
#define NLK_KB_LAYER_NAME_MAX           16U
#define NLK_KB_MAX_LAYERS               8U

struct nlk_kb_layer_desc {
    uint8_t  layer_id;             /* Layer index 0..layer_count-1 */
    uint8_t  flags;                /* 0=NORMAL, 1=TRANSIENT/MOMENTARY, 2=TOGGLE */
    char     name[NLK_KB_LAYER_NAME_MAX]; /* e.g. "Base", "FN", "PN", "Gaming" */
} __attribute__((packed));

struct nlk_kb_layers_resp {
    uint8_t  layer_count;          /* Registered layer count */
    uint8_t  active_layer;         /* Currently active layer */
    uint8_t  default_layer;        /* Default base layer */
    uint8_t  reserved;
    struct nlk_kb_layer_desc layers[NLK_KB_MAX_LAYERS];
} __attribute__((packed));

struct nlk_kb_set_active_layer_req {
    uint8_t  active_layer;
    uint8_t  reserved[3];
} __attribute__((packed));

/* Keyboard Macro Recording & Configuration */
#define NLK_KB_MAX_MACROS               8U
#define NLK_KB_MAX_MACRO_STEPS          32U
#define NLK_KB_MACRO_NAME_MAX           16U
/* Bind action "macro_play" to a key using BASE + macro slot as its keycode. */
#define NLK_KB_MACRO_KEYCODE_BASE       0xF0U
#define NLK_KB_MACRO_FLAG_RECORD_OVERFLOW 0x01U

enum nlk_kb_macro_action_type {
    NLK_KB_MACRO_ACT_NONE           = 0U,
    NLK_KB_MACRO_ACT_KEY_DOWN       = 1U, /* Key press event */
    NLK_KB_MACRO_ACT_KEY_UP         = 2U, /* Key release event */
    NLK_KB_MACRO_ACT_KEY_TAP        = 3U, /* Key tap (press + release) */
    NLK_KB_MACRO_ACT_DELAY_MS       = 4U, /* Delay interval in milliseconds */
};

struct nlk_kb_macro_step {
    uint8_t  type;                 /* enum nlk_kb_macro_action_type */
    uint8_t  key_code;             /* KC_* HID keycode */
    uint16_t delay_ms;             /* Milliseconds to wait before this action */
} __attribute__((packed));

struct nlk_kb_macro_info {
    uint8_t  macro_id;             /* Macro slot 0..NLK_KB_MAX_MACROS-1 */
    uint8_t  step_count;           /* Number of active steps (0=empty) */
    uint8_t  repeat_count;         /* Playback loops (1..255) */
    uint8_t  flags;                /* Reserved flags */
    char     name[NLK_KB_MACRO_NAME_MAX]; /* Macro description/name */
} __attribute__((packed));

struct nlk_kb_macro_list_resp {
    uint8_t  macro_count;          /* Total macro slots */
    uint8_t  recording_macro_id;   /* ID of macro currently recording (0xFF if none) */
    uint16_t reserved;
    struct nlk_kb_macro_info macros[NLK_KB_MAX_MACROS];
} __attribute__((packed));

struct nlk_kb_macro_cfg_hdr {
    uint8_t  macro_id;             /* Macro slot ID */
    uint8_t  step_count;           /* Number of steps following this header */
    uint8_t  repeat_count;         /* Playback loops */
    uint8_t  reserved;
    char     name[NLK_KB_MACRO_NAME_MAX];
} __attribute__((packed));

struct nlk_kb_macro_record_req {
    uint8_t  macro_id;             /* Target macro ID to record */
    uint8_t  record_cmd;           /* 0 = STOP, 1 = START, 2 = CLEAR */
    uint16_t reserved;
} __attribute__((packed));

#define NLK_KB_MACRO_STOP 0U
#define NLK_KB_MACRO_PLAY 1U
struct nlk_kb_macro_control_req {
    uint8_t command;
    uint8_t macro_id;
} __attribute__((packed));
struct nlk_kb_macro_status_resp {
    uint8_t playing;
    uint8_t macro_id;
    uint8_t step_index;
    uint8_t repeats_left;
    uint8_t recording_macro_id;
    uint8_t reserved[3];
} __attribute__((packed));

/* Lighting effect metadata */
struct nlk_kb_light_effect_desc {
    uint8_t effect_id;
    uint8_t reserved[3];
    char name[NLK_KB_LIGHT_EFFECT_NAME_MAX];
    char desc[NLK_KB_LIGHT_EFFECT_DESC_MAX];
} __attribute__((packed));

struct nlk_kb_light_effect_list_resp {
    uint8_t effect_count;
    uint8_t reserved[3];
    struct nlk_kb_light_effect_desc effects[NLK_KB_LIGHT_EFFECTS_PER_PAGE];
} __attribute__((packed));

/* Each page fits in the NLK payload; total_count reflects the registry. */
struct nlk_kb_light_effect_page_req {
    uint16_t start_index;
} __attribute__((packed));

struct nlk_kb_light_effect_page_resp {
    uint16_t total_count;
    uint8_t count;
    uint8_t reserved;
    struct nlk_kb_light_effect_desc effects[NLK_KB_LIGHT_EFFECTS_PER_PAGE];
} __attribute__((packed));

/* Key Action Binding & Metadata */
#define NLK_KB_MAX_ACTIONS              6U
#define NLK_KB_ACTION_NAME_MAX          16U
#define NLK_KB_ACTION_DESC_MAX          20U
#define NLK_KB_MAX_KEY_ACTIONS          4U

/* 按键动作上下文（输入按键信息） */
struct key_action_context {
    uint8_t layer;        /* 按键所在层 (0..7) */
    uint8_t key_index;    /* 按键逻辑序号 (0..key_count-1) */
    uint8_t initial_code; /* 初始配置的 HID 键码 (未经过 action 处理) */
    uint8_t key_code;     /* 当前输入按键码 (上一动作输出，初始为 initial_code) */
    uint8_t event_type;   /* 触发事件类型: 1 = 按下, 0 = 释放 */
    uint8_t state;        /* 按键物理状态: 1 = pressed, 0 = released */
    uint8_t reserved[2];
} __attribute__((packed));

/* 按键动作处理函数返回值宏定义 */
#define KEY_ACTION_PASS                 (0)     /* 动作未返回/未修改按键值 (保持已有键码继续传递) */
#define KEY_ACTION_DROP                 (-1)    /* 动作显式拦截按键 (丢弃按键，不输出 HID 报告) */

/* 动作实际处理回调函数
 * 返回值:
 *  > 0 (1..255)            : 输出了新的 HID keycode，流水线以最后一个输出的为准；
 *  0 (KEY_ACTION_PASS)     : 未返回按键值 (如仅用于点灯/旁路通知)，保持原有键码继续流转；
 *  < 0 (KEY_ACTION_DROP)   : 显式拦截按键，丢弃该按键，不产生 HID 上报。
 */
typedef int (*key_action_handler_fn_t)(
    const struct key_action_context *ctx,
    void *priv
);

/* 按键动作链结构体 */
struct nlk_kb_key_action_chain {
    uint8_t action_ids[NLK_KB_MAX_KEY_ACTIONS]; /* 绑定的局部动作 ID，按顺序触发 (0=无) */
    uint8_t count;                              /* 绑定的动作数量 (0..NLK_KB_MAX_KEY_ACTIONS) */
} __attribute__((packed));

/* 动作注册描述符 */
struct nlk_kb_action_desc {
    uint8_t  action_id;            /* 内部分配的动作 ID (1..255) */
    uint8_t  flags;                /* 属性标志: 0=默认 */
    uint16_t reserved;
    char     name[NLK_KB_ACTION_NAME_MAX]; /* 动作唯一标识字符串，如 "light_toggle", "key_light" */
    char     desc[NLK_KB_ACTION_DESC_MAX]; /* 人类可读描述，如 "Toggle RGB Light" */
} __attribute__((packed));

/* 动作列表查询响应 */
struct nlk_kb_action_list_resp {
    uint8_t  action_count;         /* Number of registered actions */
    uint8_t  reserved[3];
    struct nlk_kb_action_desc actions[NLK_KB_MAX_ACTIONS];
} __attribute__((packed));

/* 绑定动作请求 */
struct nlk_kb_bind_action_req {
    uint8_t layer;        /* 目标层 (0..7, 0xFF = 所有层) */
    uint8_t key_index;    /* 目标按键 (0..key_count-1, 0xFF = 所有按键) */
    uint8_t append;       /* 1 = 追加到动作链末尾, 0 = 清空后设置 */
    uint8_t reserved;
    char    action_name[NLK_KB_ACTION_NAME_MAX]; /* 动作名字字符串 */
} __attribute__((packed));

/* 解绑动作请求 */
struct nlk_kb_unbind_action_req {
    uint8_t layer;        /* 目标层 (0..7, 0xFF = 所有层) */
    uint8_t key_index;    /* 目标按键 (0..key_count-1, 0xFF = 所有按键) */
    uint8_t reserved[2];
    char    action_name[NLK_KB_ACTION_NAME_MAX]; /* 为空表示清空该键所有动作 */
} __attribute__((packed));

/* 查询按键动作请求 */
struct nlk_kb_get_key_actions_req {
    uint8_t layer;        /* 目标层 (0..7) */
    uint8_t key_index;    /* 目标按键 (0..key_count-1) */
    uint16_t reserved;
} __attribute__((packed));

/* 查询按键动作响应 */
struct nlk_kb_key_actions_resp {
    uint8_t action_count; /* 该按键绑定的动作数量 */
    uint8_t reserved[3];
    char    action_names[NLK_KB_MAX_KEY_ACTIONS][NLK_KB_ACTION_NAME_MAX];
} __attribute__((packed));

/* Keyboard Runtime Parameters */
struct nlk_kb_params {
    uint8_t  debounce_ms;          /* Key matrix filter debounce time (ms) */
    uint8_t  poll_rate_hz_div;     /* Rate divider: 1=1000Hz, 2=500Hz, 4=250Hz, 8=125Hz */
    uint8_t  nkro_enable;          /* 0 = 6KRO mode, 1 = Full NKRO mode */
    uint8_t  win_lock;             /* 0 = Normal, 1 = Lock Windows/GUI key */
    uint8_t  fn_lock;              /* 0 = Normal, 1 = Invert FN key modifier */
    uint8_t  sleep_timeout_min;    /* Sleep timeout (minutes, 0=never) */
    uint8_t  active_layout_id;     /* Currently active layout ID */
    uint8_t  default_layer;        /* Default base layer */
    uint16_t poll_rate_hz;         /* Polling rate in Hz (e.g. 1000) */
    uint16_t reserved;
} __attribute__((packed));

/* Multi-Configuration Profile Storage & Preset Slots */
#define NLK_KB_MAX_PROFILE_SLOTS        4U

struct nlk_kb_profile_info_resp {
    uint8_t  active_slot;          /* Active profile slot ID 0..NLK_KB_MAX_PROFILE_SLOTS-1 */
    uint8_t  slot_count;           /* Maximum supported profile slots */
    uint8_t  saved_slots_mask;     /* Bitmask of slots persisted in Flash (bit 0..bit 3) */
    uint8_t  reserved;
} __attribute__((packed));

struct nlk_kb_profile_slot_req {
    uint8_t  slot_id;              /* Target profile slot ID */
    uint8_t  reserved[3];
} __attribute__((packed));

/* Named configuration objects; file count is not limited by the protocol. */
#define NLK_KB_CONFIG_ID_SIZE       24U
#define NLK_KB_CONFIG_NAME_SIZE     32U
#define NLK_KB_CONFIG_HARDWARE_SIZE 24U
enum nlk_kb_config_kind {
    NLK_KB_CONFIG_KEYBOARD = 1U,
    NLK_KB_CONFIG_LIGHT = 2U,
    NLK_KB_CONFIG_MACRO = 3U,
};
struct nlk_kb_config_files_info {
    uint16_t version;
    uint8_t key_count;
    uint8_t layer_count;
    uint8_t macro_slots;
    uint8_t macro_steps;
    uint8_t light_zones;
    uint8_t reserved;
    char hardware[NLK_KB_CONFIG_HARDWARE_SIZE];
    char active_keyboard[NLK_KB_CONFIG_ID_SIZE];
    char active_light[NLK_KB_CONFIG_ID_SIZE];
} __attribute__((packed));
struct nlk_kb_config_file_req {
    uint8_t kind;
    uint8_t macro_id;
    uint8_t reserved[2];
    char id[NLK_KB_CONFIG_ID_SIZE];
    char name[NLK_KB_CONFIG_NAME_SIZE];
} __attribute__((packed));
struct nlk_kb_config_list_req {
    uint8_t kind;
    uint8_t reserved[3];
    uint32_t cursor;
} __attribute__((packed));
struct nlk_kb_config_list_resp {
    uint32_t next_cursor;
    uint32_t size;
    uint8_t eof;
    uint8_t reserved[3];
    char id[NLK_KB_CONFIG_ID_SIZE];
    char name[NLK_KB_CONFIG_NAME_SIZE];
} __attribute__((packed));

/* =========================================================================
 * Cap 0x0101: NLK_CAP_ENCODER — Rotary Knob / Haptic Encoder
 * ========================================================================= */
enum nlk_enc_opcode {
    NLK_ENC_OP_GET_DESC         = 0x01U,
    NLK_ENC_OP_SET_CONFIG       = 0x02U,
    NLK_ENC_EVT_CHANGE          = 0x20U,
};

struct nlk_encoder_desc {
    uint8_t  encoder_count;
    uint8_t  has_switch;
    uint8_t  has_haptic;
    uint8_t  steps_per_rev;
} __attribute__((packed));

struct nlk_encoder_cfg {
    uint8_t  encoder_id;
    uint8_t  layer;
    uint8_t  cw_keycode;
    uint8_t  ccw_keycode;
    uint8_t  press_keycode;
    uint8_t  detent_count;
    uint16_t torque_mN;
} __attribute__((packed));

struct nlk_encoder_event {
    uint8_t  encoder_id;
    uint8_t  button_state;
    int16_t  delta_steps;
    uint16_t abs_angle_deg10;
} __attribute__((packed));

/* =========================================================================
 * Cap 0x0200: NLK_CAP_DISPLAY — Screen / Sub-Display
 * ========================================================================= */
enum nlk_disp_opcode {
    NLK_DISP_OP_GET_DESC        = 0x01U,
    NLK_DISP_OP_UPDATE_STATUS   = 0x02U,
    NLK_DISP_OP_SET_BRIGHTNESS  = 0x03U,
    NLK_DISP_OP_DIRTY_RECT      = 0x10U,
    NLK_DISP_OP_UPLOAD_ASSET    = 0x11U,
};

#define NLK_PIXEL_FMT_MONO_1BPP     0U
#define NLK_PIXEL_FMT_RGB565        1U
#define NLK_PIXEL_FMT_RGB888        2U

struct nlk_disp_desc {
    uint16_t width;
    uint16_t height;
    uint8_t  pixel_format;
    uint8_t  max_fps;
    uint8_t  brightness;
    uint8_t  mode;
} __attribute__((packed));

struct nlk_disp_status_sync {
    uint8_t  active_layer;
    uint8_t  lock_leds;
    uint8_t  conn_type;
    uint8_t  conn_state;
    uint8_t  battery_pct;
    uint8_t  cpu_usage_pct;
    uint16_t current_wpm;
    uint32_t unix_timestamp;
    char     custom_label[16];
} __attribute__((packed));

struct nlk_disp_dirty_rect_hdr {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t  codec;         /* 0=RAW, 1=RLE */
    uint8_t  reserved[3];
} __attribute__((packed));

/* =========================================================================
 * Cap 0x0300: NLK_CAP_WIRELESS — BLE / 2.4G / Wi-Fi Co-processor
 * ========================================================================= */
enum nlk_wireless_opcode {
    NLK_WL_OP_GET_INFO          = 0x01U,
    NLK_WL_OP_SWITCH_CHANNEL    = 0x02U,
    NLK_WL_EVT_LINK_STATE       = 0x20U,
};

enum nlk_wl_channel {
    NLK_WL_CHAN_USB_WIRED       = 0U,
    NLK_WL_CHAN_BLE_SLOT1       = 1U,
    NLK_WL_CHAN_BLE_SLOT2       = 2U,
    NLK_WL_CHAN_BLE_SLOT3       = 3U,
    NLK_WL_CHAN_RF_24G          = 4U,
    NLK_WL_CHAN_WIFI            = 5U,
};

enum nlk_wl_conn_state {
    NLK_WL_STATE_DISCONNECTED   = 0U,
    NLK_WL_STATE_PAIRING        = 1U,
    NLK_WL_STATE_CONNECTED      = 2U,
    NLK_WL_STATE_SUSPENDED      = 3U,
};

struct nlk_wl_info_resp {
    uint8_t  sup_channels_mask;
    uint8_t  active_channel;
    uint8_t  conn_state;
    int8_t   rssi_dbm;
    uint8_t  battery_pct;
    uint8_t  host_led_locks;
    uint8_t  mac_addr[6];
} __attribute__((packed));

struct nlk_wl_switch_req {
    uint8_t  target_channel;
    uint8_t  start_pairing;
    uint8_t  clear_bonds;
    uint8_t  tx_power_level;
} __attribute__((packed));

struct nlk_wl_state_evt {
    uint8_t  active_channel;
    uint8_t  conn_state;
    int8_t   rssi_dbm;
    uint8_t  host_led_locks;
} __attribute__((packed));

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(struct nlk_pkt_hdr) == 12, "nlk_pkt_hdr must be 12 bytes");
_Static_assert(sizeof(struct nlk_frag_hdr) == 4, "nlk_frag_hdr must be 4 bytes");
_Static_assert(sizeof(struct nlk_ms_poll_hdr) == 4, "nlk_ms_poll_hdr must be 4 bytes");
_Static_assert(sizeof(struct nlk_device_desc) == 48, "nlk_device_desc must be 48 bytes");
_Static_assert(sizeof(struct nlk_cap_item) == 8, "nlk_cap_item must be 8 bytes");
_Static_assert(sizeof(struct nlk_flash_part_desc) == 28, "nlk_flash_part_desc must be 28 bytes");
_Static_assert(sizeof(struct nlk_flash_prepare_req) == 16, "nlk_flash_prepare_req must be 16 bytes");
_Static_assert(sizeof(struct nlk_flash_commit_req) == 12, "nlk_flash_commit_req must be 12 bytes");
_Static_assert(sizeof(struct nlk_flash_chunk_ack) == 8, "nlk_flash_chunk_ack must be 8 bytes");
_Static_assert(sizeof(struct nlk_vfs_open_resp) == 16, "nlk_vfs_open_resp must be 16 bytes");
_Static_assert(sizeof(struct nlk_vfs_stat_resp) == 24, "nlk_vfs_stat_resp must be 24 bytes");
_Static_assert(sizeof(struct nlk_vfs_dirent_wire) == 16, "nlk_vfs_dirent_wire must be 16 bytes");
_Static_assert(sizeof(struct nlk_log_wire_hdr) == 8, "nlk_log_wire_hdr must be 8 bytes");
_Static_assert(sizeof(struct nlk_disp_status_sync) == 28, "nlk_disp_status_sync must be 28 bytes");
_Static_assert(sizeof(struct nlk_kb_desc) == 20, "nlk_kb_desc must be 20 bytes");
_Static_assert(sizeof(struct nlk_kb_light_effect_desc) == 40, "light effect descriptor size");
_Static_assert(sizeof(struct nlk_kb_light_effect_page_resp) == 244, "light effect page size");
_Static_assert(sizeof(struct nlk_kb_light_effect_list_resp) == 244, "light effect list size");
_Static_assert(sizeof(struct nlk_kb_named_light_sync_beacon) == 24, "named light beacon size");
_Static_assert(sizeof(struct nlk_kb_light_cfg) == 8, "nlk_kb_light_cfg must be 8 bytes");
_Static_assert(sizeof(struct nlk_kb_light_sync_beacon) == 8, "nlk_kb_light_sync_beacon must be 8 bytes");
_Static_assert(sizeof(struct nlk_kb_analog_cfg) == 8, "nlk_kb_analog_cfg must be 8 bytes");
#endif

#endif /* __NOS_UAPI_NLK_H__ */
