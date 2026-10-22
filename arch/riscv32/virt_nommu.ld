/* Copyright (C) 2026 NOS contributors */
ENTRY(_start)
PHDRS { kernel PT_LOAD FLAGS(5); kernel_data PT_LOAD FLAGS(6); user PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x80000000;
 __kernel_start = .;
 .text : { KEEP(*(.boot)) */kernel/*.o(.text*) */arch/*.o(.text*) *kernel-libgcc.a:*(.text*) } :kernel
 .rodata : { */kernel/*.o(.rodata*) */arch/*.o(.rodata*) *kernel-libgcc.a:*(.rodata*) } :kernel
 . = ALIGN(4096); __kernel_ro_end = .;
 .data : { */kernel/*.o(.data* .sdata*) */arch/*.o(.data* .sdata*) *kernel-libgcc.a:*(.data* .sdata*) } :kernel_data
 .bss (NOLOAD) : { __bss_start = .; */kernel/*.o(.bss* .sbss* COMMON) */arch/*.o(.bss* .sbss* COMMON) *kernel-libgcc.a:*(.bss* .sbss* COMMON) __bss_end = .; } :kernel_data
 . = ALIGN(4096); __user_text_load = .;
 .user_text : { __user_text_start = .; */user/*.o(.text* .rodata*) */tests/guest/*.o(.text* .rodata*) */servers/*.o(.text* .rodata*) */libc/port/*.o(.text* .rodata*) *libc.a:*(.text* .rodata*) *libgcc.a:*(.text* .rodata*) . = ALIGN(4096); __user_text_end = .; } :user
 . = ALIGN(4096); __user_data_load = .;
 .user_data : { __user_data_start = .; *(.got .got.*) */user/*.o(.data* .sdata*) */tests/guest/*.o(.data* .sdata*) */servers/*.o(.data* .sdata*) */libc/port/*.o(.data* .sdata*) *libc.a:*(.data* .sdata*) *libgcc.a:*(.data* .sdata*) __user_data_end = .; } :data
 .user_bss (NOLOAD) : { __user_bss_start = .; */user/*.o(.bss* .sbss* COMMON) */tests/guest/*.o(.bss* .sbss* COMMON) */servers/*.o(.bss* .sbss* COMMON) */libc/port/*.o(.bss* .sbss* COMMON) *libc.a:*(.bss* .sbss* COMMON) *libgcc.a:*(.bss* .sbss* COMMON) . = ALIGN(16); __user_bss_end = .; } :data
 /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
