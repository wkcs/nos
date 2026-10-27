/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel fixed-size 64-byte IPC message contract.
 * Invariant: ABI size must NEVER change across #ifdef or board configs.
 */

#ifndef __NOS_UAPI_IPC_H__
#define __NOS_UAPI_IPC_H__

#include <uapi/nos/types.h>

#define NOS_IPC_INLINE_REGS  4U
#define NOS_IPC_MAX_CAPS     2U

/* IPC message flags */
#define NOS_IPC_FLAG_NONE        0x00U
#define NOS_IPC_FLAG_CAP_XFER    (1U << 0)
#define NOS_IPC_FLAG_NONBLOCK    (1U << 1)
#define NOS_IPC_FLAG_KERNEL_EVT  (1U << 2)

/* Timeout constants (in system ticks) */
#define NOS_IPC_TIMEOUT_INFINITE 0U
#define NOS_IPC_TIMEOUT_NONBLOCK 0xFFFFFFFFU

#define NOS_IPC_TAG_MAKE(opcode, flags, cap_cnt) \
    ((((uint32_t)(opcode) & 0xFFFFU) << 16) |    \
     (((uint32_t)(flags) & 0xFFU) << 8) |        \
     ((uint32_t)(cap_cnt) & 0xFFU))

#define NOS_IPC_TAG_OPCODE(tag)   (((uint32_t)(tag) >> 16) & 0xFFFFU)
#define NOS_IPC_TAG_FLAGS(tag)    (((uint32_t)(tag) >> 8) & 0xFFU)
#define NOS_IPC_TAG_CAP_CNT(tag)  ((uint32_t)(tag) & 0xFFU)

struct nos_ipc_msg {
    uint32_t     tag;                        /* opcode(16) | flags(8) | cap_count(8) */
    uint32_t     token;                      /* Kernel-injected badge/client token */
    int32_t      status;                     /* Return status (0 or -errno) */
    uint32_t     payload_len;                /* Optional out-of-line buffer byte length */
    uint64_t     regs[NOS_IPC_INLINE_REGS];  /* 32-byte inline fastpath payload */
    uint64_t     buf_addr;                   /* Optional user buffer virtual address */
    nos_handle_t caps[NOS_IPC_MAX_CAPS];     /* Up to 2 capability handles transferred */
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(struct nos_ipc_msg) == 64, "nos_ipc_msg ABI size must be 64 bytes");
#endif

#endif /* __NOS_UAPI_IPC_H__ */
