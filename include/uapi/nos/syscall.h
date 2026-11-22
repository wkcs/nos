/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 微内核系统调用号定义（0..15 核心原语 + 16..23 AI 时代原生扩展）。
 */

#ifndef __NOS_UAPI_SYSCALL_H__
#define __NOS_UAPI_SYSCALL_H__

/* 核心微内核原语（保持既有 ABI 编号完全不变） */
#define SYS_NOS_YIELD               0
#define SYS_NOS_EXIT                1
#define SYS_NOS_DEBUG_CONSOLE       2
#define SYS_NOS_CLOCK_GET           3
#define SYS_NOS_SLEEP_UNTIL         4
#define SYS_NOS_THREAD_CREATE       5
#define SYS_NOS_THREAD_CONTROL      6
#define SYS_NOS_VM_MAP              7
#define SYS_NOS_VM_UNMAP            8
#define SYS_NOS_IPC_CALL            9
#define SYS_NOS_IPC_RECV           10
#define SYS_NOS_IPC_REPLY_WAIT     11
#define SYS_NOS_NOTIFY_SIGNAL      12
#define SYS_NOS_NOTIFY_WAIT        13
#define SYS_NOS_CAP_CONTROL        14
#define SYS_NOS_IRQ_CONTROL        15

/* AI 时代原生扩展系统调用（Stage 2~4 分阶段落地实现） */
#define SYS_NOS_CELL_CONTROL       16
#define SYS_NOS_VMO_CREATE         17
#define SYS_NOS_VMO_CONTROL        18
#define SYS_NOS_RING_CREATE        19
#define SYS_NOS_RING_ENTER         20
#define SYS_NOS_SCHED_INFER        21
#define SYS_NOS_ACCEL_SUBMIT       22
#define SYS_NOS_CAP_REVOKE_TREE    23

#define SYS_NOS_MAX                24

#endif /* __NOS_UAPI_SYSCALL_H__ */
