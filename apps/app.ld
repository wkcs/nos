/* Copyright (C) 2026 NOS contributors */
ENTRY(nos_app_start)
PHDRS { headers PT_PHDR PHDRS; text PT_LOAD FILEHDR PHDRS FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); tls PT_TLS FLAGS(4); }
SECTIONS {
    . = 0x00480000 + SIZEOF_HEADERS;
    .text : { *(.text .text.*) } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = 0x01000000;
    .data : { *(.data .data.* .sdata .sdata.*) *(.got .got.*) } :data
    .tdata : { *(.tdata .tdata.*) } :data :tls
    .tbss : { *(.tbss .tbss.*) } :data :tls
    .bss : { *(.bss .bss.* .sbss .sbss.*) *(COMMON) } :data
    ASSERT(. <= 0x01006000, "ELF data exceeds private data region")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) *(.ARM.extab*) }
}
