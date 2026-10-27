/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel Syscall Dispatch Interface (16 Core Syscalls)
 */

#ifndef __NOS_SYSCALL_H__
#define __NOS_SYSCALL_H__

#include <uapi/nos/types.h>
#include <uapi/nos/syscall.h>

typedef long (*nos_syscall_fn_t)(uintptr_t a0,
                                 uintptr_t a1,
                                 uintptr_t a2,
                                 uintptr_t a3,
                                 uintptr_t a4,
                                 uintptr_t a5);

long nos_syscall_dispatch(unsigned long sysno,
                          uintptr_t a0,
                          uintptr_t a1,
                          uintptr_t a2,
                          uintptr_t a3,
                          uintptr_t a4,
                          uintptr_t a5);

#endif /* __NOS_SYSCALL_H__ */
