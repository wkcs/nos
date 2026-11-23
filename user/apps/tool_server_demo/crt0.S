/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS EL0 PIE Tool Server AArch64 启动桩 (_start -> nos_user_main -> SYS_NOS_EXIT)
 */

#define __ASSEMBLY__
#include <uapi/nos/syscall.h>

    .text
    .align 2
    .global _start
    .type _start, %function
_start:
    hint    #34
    mov     x29, #0
    mov     x30, #0
    bl      nos_user_main
    sxtw    x0, w0
    mov     x8, #SYS_NOS_EXIT
    svc     #0
1:
    wfi
    b       1b
    .size _start, .-_start
