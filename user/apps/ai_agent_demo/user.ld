/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS EL0 独立用户态 ELF64 严格页对齐 W^X 链接脚本
 */

ENTRY(_start)

PHDRS
{
    text_rx  PT_LOAD FLAGS(5); /* PF_R | PF_X */
    rodata_r PT_LOAD FLAGS(4); /* PF_R */
    data_rw  PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x0000000000400000;
    .text :
    {
        *(.text .text.*)
    } :text_rx

    . = 0x0000000000410000;
    .rodata :
    {
        *(.rodata .rodata.*)
    } :rodata_r

    . = 0x0000000000420000;
    .data :
    {
        *(.data .data.*)
    } :data_rw

    .bss :
    {
        *(.bss .bss.* COMMON)
    } :data_rw

    /DISCARD/ :
    {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}
