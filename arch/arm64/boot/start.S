/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 位置无关启动引导代码（支持 EL2→EL1 降级、双页表早期映射与跳转高半核）
 */

#define __ASSEMBLY__
#include <autocfg.h>
#include <asm/sysreg.h>
#include <asm/mmu.h>

.extern nos_arm64_boot
.extern __boot_bss_start
.extern __boot_bss_end
.extern __bss_phys_start
.extern __bss_phys_end
.extern __kernel_init_stack_top

    .section .text.boot, "ax"
    .global _start
    .type _start, %function
_start:
    /* 1. 屏蔽所有异常并保存引导传入的 DTB 物理地址与当前异常级别 */
    msr     daifset, #0xf
    mov     x20, x0                         /* x20 = DTB 物理地址 */
    mrs     x21, CurrentEL
    lsr     x21, x21, #2
    and     x21, x21, #3                    /* x21 = Boot EL (1 或 2) */

    /* 2. 检查 CPU 核号：仅主核 (Aff0 == 0) 继续引导，从核休眠等待 PSCI */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbnz    x0, secondary_park

    /* 3. 若处于 EL2，完成硬件直通配置后降级进入 EL1h */
    cmp     x21, #2
    b.ne    el1_entry

    ldr     x0, =HCR_EL2_RW                 /* EL1 强制使用 AArch64 */
    mrs     x1, id_aa64isar1_el1
    ldr     x2, =0xFF000FF0
    tst     x1, x2
    b.eq    11f
    ldr     x2, =(HCR_EL2_APK | HCR_EL2_API)
    orr     x0, x0, x2
11:
    mrs     x1, S3_0_C0_C4_1                /* ID_AA64PFR1_EL1 */
    ubfx    x1, x1, #8, #4
    cmp     x1, #1
    b.lo    12f
    ldr     x2, =HCR_EL2_ATA
    orr     x0, x0, x2
12:
    msr     hcr_el2, x0

    mov     x0, #(CNTHCTL_EL2_EL1PCTEN | CNTHCTL_EL2_EL1PCEN)
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    msr     cptr_el2, xzr                   /* 允许 EL1/EL0 访问 FP/SIMD */

    /* 若硬件支持 GICv3 系统寄存器接口，在 EL2 开放 EL1 访问权限 */
    mrs     x0, id_aa64pfr0_el1
    ubfx    x0, x0, #24, #4
    cbz     x0, 1f
    mov     x0, #0xF                        /* SRE | Enable | DIB | DFB */
    msr     SYS_ICC_SRE_EL2, x0
    isb
1:
    ldr     x0, =0x30D00800                 /* EL1 SCTLR 复位基准值（关 MMU/Cache） */
    msr     sctlr_el1, x0
    mov     x0, #(PSR_DAIF_MASK | PSR_MODE_EL1h)
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

secondary_park:
    wfe
    b       secondary_park

el1_entry:
    /* 确保使用 SP_EL1 栈指针 */
    msr     spsel, #1

    /* 4. 先清零 .bss.boot（引导栈与早期静态页表），此步尚未动用 SP */
    adrp    x0, __boot_bss_start
    add     x0, x0, :lo12:__boot_bss_start
    adrp    x1, __boot_bss_end
    add     x1, x1, :lo12:__boot_bss_end
2:
    cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b
3:
    /* 设定临时物理引导栈 */
    adrp    x0, boot_stack_top
    add     x0, x0, :lo12:boot_stack_top
    mov     sp, x0

    /* 5. 按物理 LMA 区间清零高半核主镜像 .bss */
    adrp    x0, __bss_phys_start
    add     x0, x0, :lo12:__bss_phys_start
    adrp    x1, __bss_phys_end
    add     x1, x1, :lo12:__bss_phys_end
4:
    cmp     x0, x1
    b.hs    5f
    str     xzr, [x0], #8
    b       4b
5:
    /* 6. 构建 TTBR0 临时恒等映射（0x40000000 起 32MB，2MB Block） */
    adrp    x2, boot_ttbr0_l1
    add     x2, x2, :lo12:boot_ttbr0_l1
    adrp    x3, boot_ttbr0_l2
    add     x3, x3, :lo12:boot_ttbr0_l2

#if (CONFIG_ARM64_VA_BITS == 48)
    adrp    x4, boot_ttbr0_l0
    add     x4, x4, :lo12:boot_ttbr0_l0
    orr     x5, x2, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x4]                        /* L0[0] -> L1 */
#endif
    /* 0x40000000 位于 L1 槽位 1 */
    orr     x5, x3, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x2, #(1 * 8)]

    /* 填充 boot_ttbr0_l2[0..15]：覆盖 0x40000000 ~ 0x42000000 Normal 属性 */
    ldr     x6, =0x40000000
    ldr     x7, =(ARM64_PTE_VALID | ARM64_PTE_BLOCK | ARM64_PTE_AF | ARM64_PTE_SH_INNER | ARM64_PTE_ATTRINDX(MT_NORMAL))
    mov     x8, #0
6:
    orr     x9, x6, x7
    str     x9, [x3, x8, lsl #3]
    add     x6, x6, #ARM64_BLOCK_2M_SIZE
    add     x8, x8, #1
    cmp     x8, #16
    b.lt    6b

    /* 7. 构建 TTBR1 高半核过渡映射（Physmap + MMIO + Kernel Image） */
    adrp    x2, boot_ttbr1_l1
    add     x2, x2, :lo12:boot_ttbr1_l1

#if (CONFIG_ARM64_VA_BITS == 48)
    adrp    x4, boot_ttbr1_l0
    add     x4, x4, :lo12:boot_ttbr1_l0
    /* 3 个对应高半核窗口在 48-bit 下：256(Physmap), 384(MMIO), 480(Kernel) */
    orr     x5, x2, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x4, #(256 * 8)]
    adrp    x10, boot_ttbr1_l1_mmio
    add     x10, x10, :lo12:boot_ttbr1_l1_mmio
    orr     x5, x10, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x4, #(384 * 8)]
    adrp    x11, boot_ttbr1_l1_kern
    add     x11, x11, :lo12:boot_ttbr1_l1_kern
    orr     x5, x11, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x4, #(480 * 8)]

    adrp    x3, boot_ttbr1_l2_phys
    add     x3, x3, :lo12:boot_ttbr1_l2_phys
    orr     x5, x3, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x2, #(1 * 8)]

    adrp    x3, boot_ttbr1_l2_mmio
    add     x3, x3, :lo12:boot_ttbr1_l2_mmio
    orr     x5, x3, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x10, #(0 * 8)]

    adrp    x3, boot_ttbr1_l2_kern
    add     x3, x3, :lo12:boot_ttbr1_l2_kern
    orr     x5, x3, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x11, #(448 * 8)]
#else
    /* 39-bit VA 下：Physmap 在 L1[1]，MMIO 在 L1[256]，Kernel 在 L1[448] */
    adrp    x3, boot_ttbr1_l2_phys
    add     x3, x3, :lo12:boot_ttbr1_l2_phys
    orr     x5, x3, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x2, #(1 * 8)]

    adrp    x3, boot_ttbr1_l2_mmio
    add     x3, x3, :lo12:boot_ttbr1_l2_mmio
    orr     x5, x3, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x2, #(256 * 8)]

    adrp    x3, boot_ttbr1_l2_kern
    add     x3, x3, :lo12:boot_ttbr1_l2_kern
    orr     x5, x3, #(ARM64_PTE_VALID | ARM64_PTE_TABLE)
    str     x5, [x2, #(448 * 8)]
#endif

    /* 填充 Physmap：512MB DRAM (0x40000000..0x60000000, PXN|UXN) */
    adrp    x3, boot_ttbr1_l2_phys
    add     x3, x3, :lo12:boot_ttbr1_l2_phys
    ldr     x6, =0x40000000
    ldr     x7, =(ARM64_PTE_VALID | ARM64_PTE_BLOCK | ARM64_PTE_AF | ARM64_PTE_SH_INNER | ARM64_PTE_ATTRINDX(MT_NORMAL) | ARM64_PTE_PXN | ARM64_PTE_UXN)
    mov     x8, #0
7:
    orr     x9, x6, x7
    str     x9, [x3, x8, lsl #3]
    add     x6, x6, #ARM64_BLOCK_2M_SIZE
    add     x8, x8, #1
    cmp     x8, #256
    b.lt    7b

    /* 填充 MMIO：0x08000000..0x0C000000 (L2 索引 64..95，Device-nGnRnE + PXN|UXN) */
    adrp    x3, boot_ttbr1_l2_mmio
    add     x3, x3, :lo12:boot_ttbr1_l2_mmio
    ldr     x6, =0x08000000
    ldr     x7, =(ARM64_PTE_VALID | ARM64_PTE_BLOCK | ARM64_PTE_AF | ARM64_PTE_ATTRINDX(MT_DEVICE_nGnRnE) | ARM64_PTE_PXN | ARM64_PTE_UXN)
    mov     x8, #64
8:
    orr     x9, x6, x7
    str     x9, [x3, x8, lsl #3]
    add     x6, x6, #ARM64_BLOCK_2M_SIZE
    add     x8, x8, #1
    cmp     x8, #96
    b.lt    8b

    /* 填充 Kernel Image：0x40200000 起 32MB (16 个 2MB Block，过渡映射) */
    adrp    x3, boot_ttbr1_l2_kern
    add     x3, x3, :lo12:boot_ttbr1_l2_kern
    ldr     x6, =ARM64_KERNEL_PBASE
    ldr     x7, =(ARM64_PTE_VALID | ARM64_PTE_BLOCK | ARM64_PTE_AF | ARM64_PTE_SH_INNER | ARM64_PTE_ATTRINDX(MT_NORMAL) | ARM64_PTE_UXN)
    mov     x8, #0
9:
    orr     x9, x6, x7
    str     x9, [x3, x8, lsl #3]
    add     x6, x6, #ARM64_BLOCK_2M_SIZE
    add     x8, x8, #1
    cmp     x8, #16
    b.lt    9b

    /* 8. 配置 MAIR_EL1 与 TCR_EL1（动态填入硬件 PARange 作为 IPS），装入页表根 */
    ldr     x0, =ARM64_MAIR_VALUE
    msr     mair_el1, x0

    mrs     x1, id_aa64mmfr0_el1
    and     x1, x1, #0xF                    /* PARange[3:0] */
    ldr     x0, =ARM64_TCR_BASE
    orr     x0, x0, x1, lsl #32             /* TCR_EL1.IPS */
    msr     tcr_el1, x0

#if (CONFIG_ARM64_VA_BITS == 48)
    adrp    x0, boot_ttbr0_l0
    add     x0, x0, :lo12:boot_ttbr0_l0
    adrp    x1, boot_ttbr1_l0
    add     x1, x1, :lo12:boot_ttbr1_l0
#else
    adrp    x0, boot_ttbr0_l1
    add     x0, x0, :lo12:boot_ttbr0_l1
    adrp    x1, boot_ttbr1_l1
    add     x1, x1, :lo12:boot_ttbr1_l1
#endif
    msr     ttbr0_el1, x0
    msr     ttbr1_el1, x1
    isb
    tlbi    vmalle1
    dsb     sy
    isb

    /* 9. 正式开启 MMU、指令缓存与数据缓存 */
    mrs     x0, sctlr_el1
    ldr     x1, =(SCTLR_EL1_M | SCTLR_EL1_C | SCTLR_EL1_I | SCTLR_EL1_SA | SCTLR_EL1_SA0)
    orr     x0, x0, x1
    msr     sctlr_el1, x0
    dsb     sy
    isb

    /* 10. 切换至高半核主初始化栈并绝对跳转 C 引导入口 */
    ldr     x0, =__kernel_init_stack_top
    mov     sp, x0
    mov     x0, x20                         /* 参数 0: DTB 物理地址 */
    mov     x1, x21                         /* 参数 1: 引导异常级别 */
    ldr     x8, =nos_arm64_boot
    br      x8

    /* 不应到达此处 */
    b       secondary_park

/* ---------------- ARM64 PSCI SMP 从核物理入口 ---------------- */
    .global arm64_secondary_entry
    .type arm64_secondary_entry, %function
arm64_secondary_entry:
    /* x0 = PSCI context_id (逻辑 cpu_id) */
    msr     daifset, #0xf
    mov     x20, x0

    /* 若从核由固件在 EL2 启动，先完成直通配置后降级进入 EL1h */
    mrs     x21, CurrentEL
    lsr     x21, x21, #2
    and     x21, x21, #3
    cmp     x21, #2
    b.ne    secondary_el1_entry

    ldr     x0, =HCR_EL2_RW
    mrs     x1, id_aa64isar1_el1
    ldr     x2, =0xFF000FF0
    tst     x1, x2
    b.eq    13f
    ldr     x2, =(HCR_EL2_APK | HCR_EL2_API)
    orr     x0, x0, x2
13:
    mrs     x1, S3_0_C0_C4_1                /* ID_AA64PFR1_EL1 */
    ubfx    x1, x1, #8, #4
    cmp     x1, #1
    b.lo    14f
    ldr     x2, =HCR_EL2_ATA
    orr     x0, x0, x2
14:
    msr     hcr_el2, x0
    mov     x0, #(CNTHCTL_EL2_EL1PCTEN | CNTHCTL_EL2_EL1PCEN)
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    msr     cptr_el2, xzr

    mrs     x0, id_aa64pfr0_el1
    ubfx    x0, x0, #24, #4
    cbz     x0, 10f
    mov     x0, #0xF
    msr     SYS_ICC_SRE_EL2, x0
    isb
10:
    ldr     x0, =0x30D00800
    msr     sctlr_el1, x0
    mov     x0, #(PSR_DAIF_MASK | PSR_MODE_EL1h)
    msr     spsr_el2, x0
    adr     x0, secondary_el1_entry
    msr     elr_el2, x0
    eret

secondary_el1_entry:
    msr     spsel, #1

    /* 配置 MAIR_EL1 与 TCR_EL1（动态填入 IPS） */
    ldr     x0, =ARM64_MAIR_VALUE
    msr     mair_el1, x0

    mrs     x1, id_aa64mmfr0_el1
    and     x1, x1, #0xF
    ldr     x0, =ARM64_TCR_BASE
    orr     x0, x0, x1, lsl #32
    msr     tcr_el1, x0

    /* 装入早期低半核恒等页表与主核广播的 W^X TTBR1_EL1 页表 */
#if (CONFIG_ARM64_VA_BITS == 48)
    adrp    x0, boot_ttbr0_l0
    add     x0, x0, :lo12:boot_ttbr0_l0
#else
    adrp    x0, boot_ttbr0_l1
    add     x0, x0, :lo12:boot_ttbr0_l1
#endif
    adrp    x1, arm64_smp_boot_ttbr1
    add     x1, x1, :lo12:arm64_smp_boot_ttbr1
    ldr     x1, [x1]

    /* 在开 MMU 前预先取出为本核分配的高半核内核栈顶至 x21 */
    adrp    x2, arm64_smp_boot_stacks
    add     x2, x2, :lo12:arm64_smp_boot_stacks
    ldr     x21, [x2, x20, lsl #3]

    msr     ttbr0_el1, x0
    msr     ttbr1_el1, x1
    isb
    tlbi    vmalle1
    dsb     sy
    isb

    mrs     x0, sctlr_el1
    ldr     x1, =(SCTLR_EL1_M | SCTLR_EL1_C | SCTLR_EL1_I | SCTLR_EL1_SA | SCTLR_EL1_SA0)
    orr     x0, x0, x1
    msr     sctlr_el1, x0
    dsb     sy
    isb

    mov     sp, x21
    mov     x29, #0
    mov     x30, #0
    mov     x0, x20
    ldr     x8, =arm64_secondary_main
    br      x8

    b       secondary_park
    .ltorg

/* 高半核只读导出低半核物理地址表（避免跨 4GB PC 相对重定位截断） */
    .section .rodata, "a"
    .align 3
    .global arm64_secondary_entry_phys
arm64_secondary_entry_phys:
    .quad   arm64_secondary_entry
    .global arm64_smp_boot_ttbr1_phys
arm64_smp_boot_ttbr1_phys:
    .quad   arm64_smp_boot_ttbr1
    .global arm64_smp_boot_stacks_phys
arm64_smp_boot_stacks_phys:
    .quad   arm64_smp_boot_stacks

/* ---------------- 早期引导专用低半核 BSS（页面对齐） ---------------- */
    .section .bss.boot, "aw", %nobits
    .align 12
#if (CONFIG_ARM64_VA_BITS == 48)
boot_ttbr0_l0:
    .space 4096
boot_ttbr1_l0:
    .space 4096
boot_ttbr1_l1_mmio:
    .space 4096
boot_ttbr1_l1_kern:
    .space 4096
#endif
boot_ttbr0_l1:
    .space 4096
boot_ttbr0_l2:
    .space 4096
boot_ttbr1_l1:
    .space 4096
boot_ttbr1_l2_phys:
    .space 4096
boot_ttbr1_l2_mmio:
    .space 4096
boot_ttbr1_l2_kern:
    .space 4096
    .align 4
boot_stack_bottom:
    .space 16384
boot_stack_top:
    .align 3
arm64_smp_boot_ttbr1:
    .quad 0
arm64_smp_boot_stacks:
    .space (8 * 8)
