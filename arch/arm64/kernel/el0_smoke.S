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

    .align 2
    .global arm64_el0_cell_code_start
    .global arm64_el0_cell_code_end
arm64_el0_cell_code_start:
    /*
     * x0 = EL0 只读 Telemetry 共享页虚拟地址 (0x420000)
     * 约定的 Sealed 权重 VMO 映射虚址位于 x0 + 0x10000 (0x430000)
     */
    mov     x19, x0
    add     x20, x19, #0x10, lsl #12    /* x20 = x19 + 0x10000 */

    /* 1. 零系统调用直读 Telemetry 共享页：校验 Magic / Version / 内存占用与预算上限 */
    ldr     w1, [x19, #0]
    movz    w2, #0x454C
    movk    w2, #0x4E43, lsl #16        /* 0x4E43454C ("NCEL") */
    cmp     w1, w2
    b.ne    1f

    ldrh    w2, [x19, #4]
    cmp     w2, #1
    b.ne    1f

    ldr     x3, [x19, #24]              /* mem_bytes_used >= 4096 */
    movz    x5, #4096
    cmp     x3, x5
    b.lo    1f

    ldr     x4, [x19, #64]              /* limit.mem_bytes_limit == 32768 */
    movz    x5, #0x8000
    cmp     x4, x5
    b.ne    1f

    /* 2. 校验只读共享权重 VMO 魔数 0x4E4F534149303032ULL 并通过 SVC #0 打印成功日志 */
    ldr     x1, [x20, #0]
    movz    x2, #0x3032
    movk    x2, #0x4930, lsl #16
    movk    x2, #0x5341, lsl #32
    movk    x2, #0x4E4F, lsl #48
    cmp     x1, x2
    b.ne    1f

    add     x0, x20, #16
    ldr     x1, [x20, #8]
    mov     x2, #0
    mov     x8, #SYS_NOS_DEBUG_CONSOLE
    svc     #0
    cmp     x0, #0
    b.le    1f

    /* 3. 在 EL0 创建 64KB ANON VMO（超出 32KB Cell 限额），校验 VMO_MAP 触发 -EDQUOT (-122) */
    sub     sp, sp, #32
    movz    x1, #1, lsl #16             /* size_bytes = 65536 */
    str     x1, [sp, #0]
    str     xzr, [sp, #8]               /* kind = NOS_VMO_ANON, flags = 0 */
    str     xzr, [sp, #16]
    str     xzr, [sp, #24]

    mov     x0, sp
    mov     x1, #0
    mov     x8, #SYS_NOS_VMO_CREATE
    svc     #0
    add     sp, sp, #32
    cmp     x0, #0
    b.le    1f
    mov     x21, x0                     /* x21 = new vmo_cap */

    mov     x0, #6                      /* NOS_VMO_OP_MAP */
    mov     x1, x21
    mov     x2, #0                      /* target = current->cell */
    movz    x3, #0x50, lsl #16          /* vaddr = 0x500000 */
    movz    x4, #1, lsl #16             /* len = 65536 */
    mov     x5, #0x0B                   /* USER | READ | WRITE */
    mov     x8, #SYS_NOS_VMO_CONTROL
    svc     #0
    cmn     x0, #122                    /* 必须返回 -EDQUOT (-122) */
    b.ne    1f

    /* 4. 吊销自建句柄并优雅退出 */
    mov     x0, x21
    mov     x1, #0
    mov     x8, #SYS_NOS_CAP_REVOKE_TREE
    svc     #0
    cmp     x0, #1
    b.ne    1f

    mov     x0, #0
    mov     x8, #SYS_NOS_EXIT
    svc     #0
1:
    brk     #3
    b       1b
arm64_el0_cell_code_end:

    .align 2
    .global arm64_el0_cell_tamper_code_start
    .global arm64_el0_cell_tamper_code_end
arm64_el0_cell_tamper_code_start:
    /* x0 = 只读 Telemetry 页地址：故意篡改预算页必须触发硬件 EL0 Data Abort 并记录 AUDIT_FAULT */
    str     xzr, [x0, #0]
    brk     #4
    b       arm64_el0_cell_tamper_code_start
arm64_el0_cell_tamper_code_end:
