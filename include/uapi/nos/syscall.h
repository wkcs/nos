/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel minimal syscall numbers (16 core syscalls).
 */

#ifndef __NOS_UAPI_SYSCALL_H__
#define __NOS_UAPI_SYSCALL_H__

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

#define SYS_NOS_MAX                16

#endif /* __NOS_UAPI_SYSCALL_H__ */
