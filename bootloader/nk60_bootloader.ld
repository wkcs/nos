/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * Linker Script for NK60-V2 Ultra-Compact Bootloader.
 * Confined strictly to STM32F401 Sector 0 (0x08000000 - 0x08003FFF, 16 KB),
 * leaving Sectors 1..5 (0x08004000 - 0x0803FFFF, 240 KB) exclusively for NOS.
 */

ENTRY(reset_handler)

_estack = 0x20010000; /* Top of 64 KB SRAM */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 16K
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 64K
}

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } >FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.ramfunc)
        *(.ramfunc*)
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT> FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } >RAM

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.eh_frame*)
    }
}
