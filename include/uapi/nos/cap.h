/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 微内核能力对象类型与权限掩码定义。
 */

#ifndef __NOS_UAPI_CAP_H__
#define __NOS_UAPI_CAP_H__

#include <uapi/nos/types.h>

enum nos_cap_type {
    NOS_CAP_NONE = 0,
    NOS_CAP_TASK,
    NOS_CAP_VM_SPACE,
    NOS_CAP_ENDPOINT,
    NOS_CAP_NOTIFICATION,
    NOS_CAP_MUTEX,
    NOS_CAP_IRQ,
    NOS_CAP_PHYSMEM,
    NOS_CAP_IOPORT,
    /* AI 时代原生一等内核对象类型 */
    NOS_CAP_CELL,
    NOS_CAP_VMO,
    NOS_CAP_RING,
    NOS_CAP_ACCEL,
    NOS_CAP_SNAPSHOT,
};

#define NOS_CAP_RIGHT_READ      (1U << 0)
#define NOS_CAP_RIGHT_WRITE     (1U << 1)
#define NOS_CAP_RIGHT_EXEC      (1U << 2)
#define NOS_CAP_RIGHT_SEND      (1U << 3)
#define NOS_CAP_RIGHT_RECV      (1U << 4)
#define NOS_CAP_RIGHT_GRANT     (1U << 5)
#define NOS_CAP_RIGHT_MANAGE    (1U << 6)
#define NOS_CAP_RIGHT_AUDIT     (1U << 7)
#define NOS_CAP_RIGHT_MAP       (1U << 8)

#define NOS_CAP_RIGHT_ALL       0x1FFU

/* Capability control operations for SYS_NOS_CAP_CONTROL */
#define NOS_CAP_OP_DERIVE       1U
#define NOS_CAP_OP_REVOKE       2U
#define NOS_CAP_OP_INSPECT      3U

/* IRQ control operations for SYS_NOS_IRQ_CONTROL */
#define NOS_IRQ_OP_BIND         1U
#define NOS_IRQ_OP_UNBIND       2U
#define NOS_IRQ_OP_ACK          3U

#endif /* __NOS_UAPI_CAP_H__ */
