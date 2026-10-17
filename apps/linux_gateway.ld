/* Copyright (C) 2026 NOS contributors */
ENTRY(nos_linux_gateway)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x01800000;
 .text : { *(.text*) *(.rodata*) } :code
 . = ALIGN(4096);
 .data : { *(.data* .sdata* .got*) } :data
 .bss (NOLOAD) : { *(.bss* .sbss* COMMON) } :data
 ASSERT(. <= 0x01900000, "Linux gateway exceeds its reserved window")
 /DISCARD/ : { *(.comment .note* .eh_frame* .ARM.exidx* .ARM.extab*) }
}
