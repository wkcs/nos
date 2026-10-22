/* Copyright (C) 2026 NOS contributors */
ENTRY(_start)
MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1024K
 RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K }
SECTIONS {
 .vectors : { KEEP(*(.vectors)) } > FLASH
 .text : { KEEP(*(.boot)) */kernel/*.o(.text* .rodata*) */arch/*.o(.text* .rodata*) *kernel-libgcc.a:*(.text* .rodata*) } > FLASH
 .user_text : { __user_text_start = .; */user/*.o(.text* .rodata*) */tests/guest/*.o(.text* .rodata*) */servers/*.o(.text* .rodata*) */libc/port/*.o(.text* .rodata*) *libc.a:*(.text* .rodata*) *libgcc.a:*(.text* .rodata*) __user_text_end = .; } > FLASH
 __user_text_load = LOADADDR(.user_text);
 .data : { __data_start = .; */kernel/*.o(.data* .sdata*) */arch/*.o(.data* .sdata*) *kernel-libgcc.a:*(.data* .sdata*) __data_end = .; } > RAM AT> FLASH
 __data_load = LOADADDR(.data);
 .bss (NOLOAD) : { __bss_start = .; */arch/*.o(.bss* .sbss* COMMON) *kernel-libgcc.a:*(.bss* .sbss* COMMON) */kernel/*.o(.bss* .sbss* COMMON) __bss_end = .; } > RAM
 .user_data ALIGN(16384) : { __user_data_start = .; *(.got .got.*) */user/*.o(.data* .sdata*) */tests/guest/*.o(.data* .sdata*) */servers/*.o(.data* .sdata*) */libc/port/*.o(.data* .sdata*) *libc.a:*(.data* .sdata*) *libgcc.a:*(.data* .sdata*) __user_data_end = .; } > RAM AT> FLASH
 __user_data_load = LOADADDR(.user_data);
 .user_bss __user_data_end (NOLOAD) : { __user_bss_start = .; */user/*.o(.bss* .sbss* COMMON) */tests/guest/*.o(.bss* .sbss* COMMON) */servers/*.o(.bss* .sbss* COMMON) */libc/port/*.o(.bss* .sbss* COMMON) *libc.a:*(.bss* .sbss* COMMON) *libgcc.a:*(.bss* .sbss* COMMON) __user_bss_end = .; } > RAM
 __stack_top = ORIGIN(RAM) + LENGTH(RAM);
 ASSERT(__user_bss_end + 4096 < __stack_top, "STM32 RAM exhausted")
 /DISCARD/ : { *(.comment .note* .eh_frame* .ARM.exidx* .ARM.extab*) }
}
