/* Copyright (C) 2026 NOS contributors */
ENTRY(_start)
NOS_USER_TEMPLATE_LIMIT = DEFINED(NOS_USER_TEMPLATE_LIMIT) ? NOS_USER_TEMPLATE_LIMIT : 24576;
PHDRS { kernel PT_LOAD FLAGS(5); kernel_data PT_LOAD FLAGS(6); user PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x40010000;
 __kernel_start = .;
 .text : { KEEP(*(.boot)) KEEP(*(.vectors)) */kernel/micro/*.o(.text*) */arch/*/micro/*.o(.text*) *kernel-libgcc.a:*(.text*) } :kernel
 .rodata : { */kernel/micro/*.o(.rodata*) */arch/*/micro/*.o(.rodata*) *kernel-libgcc.a:*(.rodata*) } :kernel
 . = ALIGN(4096);
 __kernel_ro_end = .;
 .data : { */kernel/micro/*.o(.data* .sdata*) */arch/*/micro/*.o(.data* .sdata*) *kernel-libgcc.a:*(.data* .sdata*) } :kernel_data
 .bss (NOLOAD) : { __bss_start = .; */kernel/micro/*.o(.bss* .sbss* COMMON) */arch/*/micro/*.o(.bss* .sbss* COMMON) *kernel-libgcc.a:*(.bss* .sbss* COMMON) __bss_end = .; } :kernel_data
 __user_text_load = ALIGN(., 4096);
 .user_text 0x00400000 : AT(__user_text_load) {
   __user_text_start = .;
   */user/*.o(.text* .rodata*) */servers/*.o(.text* .rodata*) */libc/port/*.o(.text* .rodata*)
   *libc.a:*(.text* .rodata*) *libgcc.a:*(.text* .rodata*)
   . = ALIGN(4096); __user_text_end = .;
 } :user
 __user_data_load = ALIGN(__user_text_load + SIZEOF(.user_text), 4096);
 .user_data 0x01000000 : AT(__user_data_load) {
   __user_data_start = .; *(.got .got.*)
   */user/*.o(.data* .sdata*) */servers/*.o(.data* .sdata*) */libc/port/*.o(.data* .sdata*)
   *libc.a:*(.data* .sdata*) *libgcc.a:*(.data* .sdata*)
   __user_data_end = .;
 } :data
 .user_bss (0x01000000 + SIZEOF(.user_data)) (NOLOAD) : { __user_bss_start = .; */user/*.o(.bss* .sbss* COMMON) */servers/*.o(.bss* .sbss* COMMON) */libc/port/*.o(.bss* .sbss* COMMON) *libc.a:*(.bss* .sbss* COMMON) *libgcc.a:*(.bss* .sbss* COMMON) __user_bss_end = .; } :data
 ASSERT(__user_bss_end - __user_data_start <= NOS_USER_TEMPLATE_LIMIT, "user template exceeds reserved data area")
 /DISCARD/ : { *(.comment .note* .eh_frame* .ARM.exidx* .ARM.extab*) }
}
