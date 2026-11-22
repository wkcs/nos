/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * QEMU Virt ARM64 Higher-Half 链接脚本
 * 特性：
 *   1. 低半核引导段 (.boot) VMA == LMA = 0x40200000（保护 QEMU DTB 所在 0x40000000 区）；
 *   2. 高半核主镜像段 VMA = 0xFFFFFFF000000000 + 物理偏移，LMA 连续紧邻 0x40200000+，
 *      确保 nos.bin 体积紧凑且 objcopy -Oihex 全部处于 32 位物理地址范围内；
 *   3. 严格 4KB 页隔离 + 独立 PHDRS，保障 .text(R-X) / .rodata(R--) / .data+.bss(RW-) 硬件 W^X。
 */

#include <autocfg.h>

#ifndef CONFIG_PAGE_SIZE
#define CONFIG_PAGE_SIZE 4096
#endif

#ifndef CONFIG_ARM64_VA_BITS
#define CONFIG_ARM64_VA_BITS 39
#endif

#define ARM64_KERNEL_PHYS_BASE      0x40200000
#define ARM64_KERNEL_VIRT_BASE      0xFFFFFFF000000000
#if (CONFIG_ARM64_VA_BITS == 48)
#define ARM64_PHYSMAP_BASE          0xFFFF800000000000
#else
#define ARM64_PHYSMAP_BASE          0xFFFFFF8000000000
#endif

#define ARM64_VIRT_TO_PHYS_OFF      (ARM64_KERNEL_VIRT_BASE - ARM64_KERNEL_PHYS_BASE)
#define QEMU_VIRT_DRAM_BASE         0x40000000
#define QEMU_VIRT_DRAM_POOL_SIZE    (64 * 1024 * 1024)

ENTRY(_start)

PHDRS
{
    boot_rx   PT_LOAD FLAGS(5); /* R | X */
    boot_rw   PT_LOAD FLAGS(6); /* R | W */
    text_rx   PT_LOAD FLAGS(5); /* R | X */
    rodata_r  PT_LOAD FLAGS(4); /* R */
    data_rw   PT_LOAD FLAGS(6); /* R | W */
}

SECTIONS
{
    . = ARM64_KERNEL_PHYS_BASE;

    /* ---------------- 1. 物理地址早期引导区 ---------------- */
    .boot.text :
    {
        *(.text.boot*)
        *(.rodata.boot*)
    } :boot_rx

    . = ALIGN(CONFIG_PAGE_SIZE);
    .boot.bss (NOLOAD) :
    {
        __boot_bss_start = .;
        *(.bss.boot*)
        . = ALIGN(CONFIG_PAGE_SIZE);
        __boot_bss_end = .;
    } :boot_rw

    . = ALIGN(65536);
    __kernel_boot_phys_end = .;

    /* ---------------- 2. 高半核主镜像区 (VMA 与 LMA 解耦) ---------------- */
    . = ARM64_KERNEL_VIRT_BASE + (__kernel_boot_phys_end - ARM64_KERNEL_PHYS_BASE);

    .text : AT(ADDR(.text) - ARM64_VIRT_TO_PHYS_OFF)
    {
        __kernel_text_start = .;
        _text = .;

        . = ALIGN(2048);
        KEEP(*(.vectors))

        *(.text .text.*)
        *(.kernel_init)

        . = ALIGN(8);
        __task_data_start = .;
        KEEP(*(.task.data))
        __task_data_end = .;

        . = ALIGN(8);
        __sys_init_start = .;
        KEEP(*(.sys_init.data))
        __sys_init_end = .;

        . = ALIGN(8);
        __console_data_start = .;
        KEEP(*(.console.data))
        __console_data_end = .;

        . = ALIGN(8);
        __init_data_start = .;
        KEEP(*(.core_init.data))
        KEEP(*(.early_init.data))
        KEEP(*(.normal_init.data))
        KEEP(*(.late_init.data))
        __init_data_end = .;

        . = ALIGN(CONFIG_PAGE_SIZE);
        __kernel_text_end = .;
        _etext = .;
    } :text_rx

    .rodata : AT(ADDR(.rodata) - ARM64_VIRT_TO_PHYS_OFF)
    {
        __kernel_rodata_start = .;
        *(.rodata .rodata.*)

        . = ALIGN(8);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(.fini_array*))
        KEEP(*(SORT(.fini_array.*)))
        PROVIDE_HIDDEN(__fini_array_end = .);

        . = ALIGN(CONFIG_PAGE_SIZE);
        __kernel_rodata_end = .;
    } :rodata_r

    .data : AT(ADDR(.data) - ARM64_VIRT_TO_PHYS_OFF)
    {
        __kernel_data_start = .;
        _sdata = .;
        *(.data .data.*)

        . = ALIGN(8);
        __memory_node_data_start = .;
        KEEP(*(.memory_node.data))
        __memory_node_data_end = .;

        . = ALIGN(8);
        _edata = .;
        __kernel_data_end = .;
    } :data_rw

    . = ALIGN(16);
    .bss (NOLOAD) : AT(ADDR(.bss) - ARM64_VIRT_TO_PHYS_OFF)
    {
        __kernel_bss_start = .;
        _sbss = .;
        __bss_start__ = _sbss;
        __bss_phys_start = . - ARM64_VIRT_TO_PHYS_OFF;

        *(.bss .bss.*)
        *(COMMON)

        /* 32KB 高半核主引导栈 */
        . = ALIGN(16);
        __kernel_init_stack_bottom = .;
        . += 0x8000;
        __kernel_init_stack_top = .;

        . = ALIGN(CONFIG_PAGE_SIZE);
        _ebss = .;
        __bss_end__ = _ebss;
        __kernel_bss_end = .;
        __bss_phys_end = . - ARM64_VIRT_TO_PHYS_OFF;
    } :data_rw

    PROVIDE(end = _ebss);
    PROVIDE(_end = _ebss);

    /* ---------------- 3. 位于线性直映区的动态页池边界 ---------------- */
    __mm_sys_reserve_start = ARM64_PHYSMAP_BASE + ARM64_KERNEL_PHYS_BASE;
    __mm_sys_reserve_end   = ARM64_PHYSMAP_BASE + __bss_phys_end;
    __mm_pool_start        = ARM64_PHYSMAP_BASE + __bss_phys_end;
    __mm_pool_end          = ARM64_PHYSMAP_BASE + QEMU_VIRT_DRAM_BASE + QEMU_VIRT_DRAM_POOL_SIZE;

    /DISCARD/ :
    {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
        *(.ARM*)
    }
}
