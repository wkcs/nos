/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS EL0 用户态 AArch64 最小启动桩 (_start -> nos_user_main -> SYS_NOS_EXIT)
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
    /* 返回值 w0 直接作为 SYS_NOS_EXIT 退出码 */
    sxtw    x0, w0
    mov     x8, #SYS_NOS_EXIT
    svc     #0
1:
    wfi
    b       1b
    .size _start, .-_start
