/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 原生 AArch64 EL0 用户态硬件验证载荷（位置无关指令片段）
 * 用于在真实硬件 EL0t 下验证用户栈、SVC #0、内核指针防伪拦截及 W^X 数据中止隔离。
 */

#define __ASSEMBLY__
#include <uapi/nos/syscall.h>

    .text
    .align 2
    .global arm64_el0_smoke_code_start
    .global arm64_el0_smoke_code_end
arm64_el0_smoke_code_start:
    /* x0 = 只读数据页 EL0 虚拟地址 [magic(8B), str_len(8B), str(...)] */
    mov     x19, x0

    /* 1. 验证 EL0 用户栈 (SP_EL0) 读写正常 */
    sub     sp, sp, #32
    mov     x1, #0x5A
    str     x1, [sp, #0]
    ldr     x2, [sp, #0]
    cmp     x1, x2
    b.ne    1f

    /* 2. 校验只读数据页魔数 0x4E4F534149303031ULL */
    ldr     x1, [x19, #0]
    movz    x2, #0x3031
    movk    x2, #0x4930, lsl #16
    movk    x2, #0x5341, lsl #32
    movk    x2, #0x4E4F, lsl #48
    cmp     x1, x2
    b.ne    1f

    /* 3. SVC #0 调用 SYS_NOS_CLOCK_GET 写回栈内存 */
    str     xzr, [sp, #8]
    add     x0, sp, #8
    mov     x8, #SYS_NOS_CLOCK_GET
    svc     #0
    cbnz    x0, 1f
    ldr     x2, [sp, #8]
    cbz     x2, 1f

    /* 4. 边界攻击验证：传入 EL1 高半核指针调用 SYS_NOS_DEBUG_CONSOLE，必须被拦截返回 -EFAULT (-14) */
    mov     x0, #-1
    lsl     x0, x0, #36
    mov     x3, #0x1000
    add     x0, x0, x3
    mov     x1, #16
    mov     x2, #0
    mov     x8, #SYS_NOS_DEBUG_CONSOLE
    svc     #0
    cmn     x0, #14
    b.ne    1f

    /* 5. 合法调用 SYS_NOS_DEBUG_CONSOLE 打印只读页消息 "[EL0] Hello via SVC #0!\n" */
    add     x0, x19, #16
    ldr     x1, [x19, #8]
    mov     x2, #0
    mov     x8, #SYS_NOS_DEBUG_CONSOLE
    svc     #0
    cmp     x0, #0
    b.le    1f

    /* 6. 调用 SYS_NOS_EXIT 优雅退出 */
    mov     x0, #0
    mov     x8, #SYS_NOS_EXIT
    svc     #0
1:
    brk     #1
    b       1b
arm64_el0_smoke_code_end:

    .align 2
    .global arm64_el0_fault_code_start
    .global arm64_el0_fault_code_end
arm64_el0_fault_code_start:
    /* x0 = 无写权限的只读页地址：故意写入触发硬件 EL0 Data Abort */
    str     xzr, [x0, #0]
    brk     #2
    b       arm64_el0_fault_code_start
arm64_el0_fault_code_end:
