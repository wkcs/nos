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
    NLK_CAP_FW_UPDATE   = 0x0001U,
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
    NLK_CORE_EVT_TOPO_CHANGE    = 0x20U,
};

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
 * Cap 0x0001: NLK_CAP_FW_UPDATE — Firmware OTA / DFU
 * ========================================================================= */
enum nlk_fw_opcode {
    NLK_FW_OP_GET_PART_INFO     = 0x01U,
    NLK_FW_OP_PREPARE           = 0x02U,
    NLK_FW_OP_WRITE_CHUNK       = 0x03U,
    NLK_FW_OP_VERIFY_COMMIT     = 0x04U,
    NLK_FW_OP_REBOOT            = 0x05U,
};

#define NLK_FW_MAX_PARTS            4U

#define NLK_FW_PART_SLOT0           0U
#define NLK_FW_PART_SLOT1           1U
#define NLK_FW_PART_SLOT2           2U
#define NLK_FW_PART_SLOT3           3U

struct nlk_fw_part_info_resp {
    uint8_t  active_part;
    uint8_t  part_count;
    uint16_t max_chunk_size;
    uint16_t max_window_pkts;
    uint16_t reserved;
    uint32_t running_version;
    uint32_t part_max_size[NLK_FW_MAX_PARTS];
} __attribute__((packed));

struct nlk_fw_prepare_req {
    uint8_t  part_index;
    uint8_t  flags;
    uint16_t window_pkts;
    uint32_t total_size;
    uint32_t image_crc32;
    uint32_t version_code;
} __attribute__((packed));

struct nlk_fw_chunk_hdr {
    uint32_t offset;
} __attribute__((packed));

struct nlk_fw_chunk_ack {
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
    NLK_SHELL_STREAM_STDIN      = 0x10U,
    NLK_SHELL_STREAM_STDOUT     = 0x11U,
};

#define NLK_SHELL_SIG_INT           2U
#define NLK_SHELL_SIG_QUIT          3U

struct nlk_shell_open_msg {
    uint16_t cols;
    uint16_t rows;
    uint16_t initial_credit;
    uint16_t flags;
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
    NLK_KB_STREAM_LIGHT_FRAME       = 0x10U,
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

/* Keyboard Lighting Effects (bound to NLK_CAP_KEYBOARD) */
enum nlk_kb_light_effect {
    NLK_KB_LIGHT_EFF_OFF            = 0U,
    NLK_KB_LIGHT_EFF_STATIC         = 1U,
    NLK_KB_LIGHT_EFF_BREATH         = 2U,
    NLK_KB_LIGHT_EFF_GRADIENT_WAVE  = 3U,
    NLK_KB_LIGHT_EFF_REACTIVE       = 4U,
    NLK_KB_LIGHT_EFF_RIPPLE         = 5U,
    NLK_KB_LIGHT_EFF_DIRECT_STREAM  = 6U,
};

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
    uint8_t  effect_id;         /* enum nlk_kb_light_effect */
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

struct nlk_kb_light_sync_beacon {
    uint32_t master_frame_tick;
    uint8_t  effect_id;
    uint8_t  speed;
    uint8_t  brightness;
    uint8_t  hue_offset;
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
_Static_assert(sizeof(struct nlk_fw_prepare_req) == 16, "nlk_fw_prepare_req must be 16 bytes");
_Static_assert(sizeof(struct nlk_fw_chunk_ack) == 8, "nlk_fw_chunk_ack must be 8 bytes");
_Static_assert(sizeof(struct nlk_log_wire_hdr) == 8, "nlk_log_wire_hdr must be 8 bytes");
_Static_assert(sizeof(struct nlk_disp_status_sync) == 28, "nlk_disp_status_sync must be 28 bytes");
_Static_assert(sizeof(struct nlk_kb_desc) == 20, "nlk_kb_desc must be 20 bytes");
_Static_assert(sizeof(struct nlk_kb_light_cfg) == 8, "nlk_kb_light_cfg must be 8 bytes");
_Static_assert(sizeof(struct nlk_kb_light_sync_beacon) == 8, "nlk_kb_light_sync_beacon must be 8 bytes");
_Static_assert(sizeof(struct nlk_kb_analog_cfg) == 8, "nlk_kb_analog_cfg must be 8 bytes");
#endif

#endif /* __NOS_UAPI_NLK_H__ */
