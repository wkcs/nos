/* Copyright (C) 2026 NOS contributors */
ENTRY(nos_app_start)
PHDRS { headers PT_PHDR PHDRS; text PT_LOAD FILEHDR PHDRS FLAGS(5); data PT_LOAD FLAGS(6); dynamic PT_DYNAMIC FLAGS(6); }
SECTIONS {
    . = SIZEOF_HEADERS;
    .text : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    .hash : { *(.hash) *(.gnu.hash) } :text
    .dynsym : { *(.dynsym) } :text
    .dynstr : { *(.dynstr) } :text
    .rel.dyn : { *(.rel.dyn) *(.rel.*) } :text
    . = ALIGN(4096);
    .data : { *(.data .data.*) *(.got .got.*) } :data
    .dynamic : { *(.dynamic) } :data :dynamic
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) *(.ARM.extab*) }
}
