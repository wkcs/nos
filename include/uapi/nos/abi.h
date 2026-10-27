/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel ABI version and service protocol tags.
 */

#ifndef __NOS_UAPI_ABI_H__
#define __NOS_UAPI_ABI_H__

#include <uapi/nos/types.h>
#include <uapi/nos/errno.h>
#include <uapi/nos/syscall.h>
#include <uapi/nos/ipc.h>
#include <uapi/nos/cap.h>

#define NOS_ABI_MAJOR           1U
#define NOS_ABI_MINOR           0U
#define NOS_ABI_VERSION         ((NOS_ABI_MAJOR << 16) | NOS_ABI_MINOR)

/* Well-known root bootstrap capability slots */
#define NOS_CAP_SLOT_SELF_TASK  1U
#define NOS_CAP_SLOT_SELF_VMS   2U
#define NOS_CAP_SLOT_CONSOLE    3U
#define NOS_CAP_SLOT_NAME_EP    4U
#define NOS_CAP_SLOT_PROC_EP    5U
#define NOS_CAP_SLOT_VFS_EP     6U

/* Process service IPC opcodes */
#define NOS_PROC_OP_GETPID      1U
#define NOS_PROC_OP_SPAWN       2U
#define NOS_PROC_OP_EXIT        3U
#define NOS_PROC_OP_WAITPID     4U
#define NOS_PROC_OP_KILL        5U

/* VFS service IPC opcodes */
#define NOS_VFS_OP_OPEN         1U
#define NOS_VFS_OP_CLOSE        2U
#define NOS_VFS_OP_READ         3U
#define NOS_VFS_OP_WRITE        4U
#define NOS_VFS_OP_LSEEK        5U
#define NOS_VFS_OP_FSTAT        6U
#define NOS_VFS_OP_IOCTL        7U
#define NOS_VFS_OP_MKDIR        8U
#define NOS_VFS_OP_UNLINK       9U
#define NOS_VFS_OP_MOUNT       10U

#endif /* __NOS_UAPI_ABI_H__ */
