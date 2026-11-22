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

    .align 2
    .global arm64_el0_neon_worker_start
    .global arm64_el0_neon_worker_end
arm64_el0_neon_worker_start:
    /* x0 = 32-bit 向量种子模式（与竞态并发任务区分校验惰性 NEON 上下文完整性） */
    mov     w19, w0
    dup     v0.4s, w19
    add     w1, w19, #1
    dup     v1.4s, w1
    add     w2, w19, #2
    dup     v2.4s, w2
    add     w3, w19, #3
    dup     v3.4s, w3

    mov     w20, #8
2:
    mov     x8, #SYS_NOS_YIELD
    svc     #0

    umov    w4, v0.s[0]
    cmp     w4, w19
    b.ne    1f
    umov    w4, v0.s[3]
    cmp     w4, w19
    b.ne    1f

    add     w1, w19, #1
    umov    w4, v1.s[1]
    cmp     w4, w1
    b.ne    1f

    add     w2, w19, #2
    umov    w4, v2.s[2]
    cmp     w4, w2
    b.ne    1f

    add     w3, w19, #3
    umov    w4, v3.s[3]
    cmp     w4, w3
    b.ne    1f

    subs    w20, w20, #1
    b.ne    2b

    mov     x0, #0
    mov     x8, #SYS_NOS_EXIT
    svc     #0
1:
    brk     #5
    b       1b
arm64_el0_neon_worker_end:

    .align 2
    .global arm64_el0_stage3_code_start
    .global arm64_el0_stage3_code_end
arm64_el0_stage3_code_start:
    /*
     * x0 = RingIPC 共享环页 EL0 虚址 (0x440000)
     * x0 + 0x10000 (0x450000) 为 Snapshot 保护的可写 ANON 工作区 VMO：
     *   [+0]: "BASE_STATE_V1", [+256]: ring_cap, [+264]: msg_len, [+272]: 成功日志字符串
     */
    mov     x19, x0
    add     x20, x19, #0x10, lsl #12

    /* 1. 直接向 Snapshot 降权只读的工作区页写入，验证硬件 EL0 Data Abort 触发透明 CoW 页断裂恢复可写 */
    movz    x1, #0x4944
    movk    x1, #0x5452, lsl #16
    movk    x1, #0x0059, lsl #32
    str     x1, [x20, #0]
    ldr     x2, [x20, #0]
    cmp     x1, x2
    b.ne    1f

    /* 2. 读取 RingIPC 头布局并写入 2 条 SQE（#0 白名单+截止期继承，#1 未授权 Schema 拦截） */
    ldr     x21, [x20, #256]            /* ring_cap */
    ldr     w22, [x19, #28]             /* sq_offset */
    add     x23, x19, x22               /* SQ[0] */
    ldr     w24, [x19, #76]             /* cq_offset */
    add     x25, x19, x24               /* CQ[0] */
    add     x9, x23, #64                /* SQ[1] */

    /* 清零 SQ[0] 与 SQ[1] */
    mov     w3, #0
3:
    str     xzr, [x23, x3]
    str     xzr, [x9, x3]
    add     w3, w3, #8
    cmp     w3, #64
    b.lo    3b

    /* SQ[0]: opcode=1(TOOL_CALL), flags=8(DEADLINE), req_id=0x100, schema=0x544F4F4C0001, dl=2ms */
    mov     w1, #1
    strh    w1, [x23, #0]
    mov     w1, #8
    strh    w1, [x23, #2]
    mov     x1, #0x100
    str     x1, [x23, #8]
    movz    x1, #0x0001
    movk    x1, #0x4F4C, lsl #16
    movk    x1, #0x544F, lsl #32
    str     x1, [x23, #16]
    movz    x1, #0x8480
    movk    x1, #0x001E, lsl #16        /* 2,000,000 ns (2ms) */
    str     x1, [x23, #40]

    /* SQ[1]: opcode=1(TOOL_CALL), req_id=0x101, schema=0xBAD000000001（未授权） */
    mov     w1, #1
    strh    w1, [x9, #0]
    mov     x1, #0x101
    str     x1, [x9, #8]
    movz    x1, #1
    movk    x1, #0xBAD0, lsl #32
    str     x1, [x9, #16]

    /* stlr 释放屏障发布 sq_tail = 2 */
    ldr     w26, [x19, #20]
    add     w26, w26, #2
    add     x27, x19, #20
    stlr    w26, [x27]

    /* 调用 SYS_NOS_RING_ENTER(ring_cap, to_submit=2, min_complete=2, SUBMIT|WAIT_CQ, INFINITE=0) */
    mov     x0, x21
    mov     x1, #2
    mov     x2, #2
    mov     x3, #3
    mov     w4, #0                      /* NOS_IPC_TIMEOUT_INFINITE = 0U */
    mov     x8, #SYS_NOS_RING_ENTER
    svc     #0
    cmp     x0, #2
    b.ne    1f

    /* ldar 获取屏障等待 CQ 中至少有 2 条记录，随后按 req_id 乱序匹配 CQE[0..1] */
    add     x27, x19, #68
    ldar    w1, [x27]
    cmp     w1, #2
    b.lo    1f

    mov     w4, #0
    mov     x5, x25
    mov     w6, #2
4:
    ldr     x7, [x5, #0]
    cmp     x7, #0x100
    b.ne    5f
    ldr     w1, [x5, #8]
    cbnz    w1, 1f
    ldr     w1, [x5, #16]
    movz    w2, #0x5A5A
    cmp     w1, w2
    b.ne    1f
    orr     w4, w4, #1
    b       6f
5:
    cmp     x7, #0x101
    b.ne    1f
    ldr     w1, [x5, #8]
    cmn     w1, #13
    b.ne    1f
    orr     w4, w4, #2
6:
    add     x5, x5, #32
    subs    w6, w6, #1
    b.ne    4b

    cmp     w4, #3
    b.ne    1f

    /* 3. 调用 SYS_NOS_SCHED_INFER(NOS_SCHED_OP_QUERY_TOPO) 校验硬件算力拓扑 */
    sub     sp, sp, #32
    mov     x0, #4                      /* NOS_SCHED_OP_QUERY_TOPO */
    mov     x1, #0
    mov     x2, sp
    mov     x8, #SYS_NOS_SCHED_INFER
    svc     #0
    cbnz    x0, 1f
    ldr     w1, [sp, #0]                /* nr_cpus >= 1 */
    cmp     w1, #1
    b.lo    1f
    ldr     w1, [sp, #4]                /* online_mask != 0 */
    cbz     w1, 1f
    add     sp, sp, #32

    /* 4. 打印 Stage 3 端到端通过日志并退出 */
    add     x0, x20, #272
    ldr     x1, [x20, #264]
    mov     x2, #0
    mov     x8, #SYS_NOS_DEBUG_CONSOLE
    svc     #0
    cmp     x0, #0
    b.le    1f

    mov     x0, #0
    mov     x8, #SYS_NOS_EXIT
    svc     #0
1:
    brk     #6
    b       1b
arm64_el0_stage3_code_end:
