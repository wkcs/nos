/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS EL0 独立用户态 ET_DYN PIE 严格页隔离 W^X 链接脚本
 */

ENTRY(_start)

PHDRS
{
    text_rx  PT_LOAD FLAGS(5); /* PF_R | PF_X */
    rodata_r PT_LOAD FLAGS(4); /* PF_R */
    data_rw  PT_LOAD FLAGS(6); /* PF_R | PF_W */
    dynamic  PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
    . = 0x0000000000000000;
    .text :
    {
        *(.text .text.*)
        *(.plt .plt.*)
    } :text_rx

    . = 0x0000000000010000;
    .rodata :
    {
        *(.rodata .rodata.*)
    } :rodata_r

    .rela.dyn :
    {
        *(.rela.init)
        *(.rela.text .rela.text.*)
        *(.rela.rodata .rela.rodata.*)
        *(.rela.data .rela.data.*)
        *(.rela.got)
        *(.rela.plt)
        *(.rela.*)
    } :rodata_r

    .hash :
    {
        *(.hash)
    } :rodata_r

    .gnu.hash :
    {
        *(.gnu.hash)
    } :rodata_r

    .dynsym :
    {
        *(.dynsym .dynsym.*)
    } :rodata_r

    .dynstr :
    {
        *(.dynstr .dynstr.*)
    } :rodata_r

    . = 0x0000000000020000;
    .dynamic :
    {
        *(.dynamic)
    } :data_rw :dynamic

    .got :
    {
        *(.got)
    } :data_rw

    .got.plt :
    {
        *(.got.plt)
    } :data_rw

    .data.rel.ro :
    {
        *(.data.rel.ro .data.rel.ro.*)
    } :data_rw

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
        *(.gnu.version*)
        *(.interp)
    }
}
