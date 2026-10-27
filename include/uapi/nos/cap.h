/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel capability types and permission rights.
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
};

#define NOS_CAP_RIGHT_READ      (1U << 0)
#define NOS_CAP_RIGHT_WRITE     (1U << 1)
#define NOS_CAP_RIGHT_EXEC      (1U << 2)
#define NOS_CAP_RIGHT_SEND      (1U << 3)
#define NOS_CAP_RIGHT_RECV      (1U << 4)
#define NOS_CAP_RIGHT_GRANT     (1U << 5)
#define NOS_CAP_RIGHT_MANAGE    (1U << 6)

#endif /* __NOS_UAPI_CAP_H__ */
