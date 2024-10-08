#include <autocfg.h>

/* Entry Point */
ENTRY(reset_handler)

/* Highest address of the user mode stack */
_estack = 0x20010000;    /* end of 64K RAM */

/* Generate a link error if heap and stack don't fit into RAM */
_Min_Heap_Size = 0;      /* required amount of heap  */
_Min_Stack_Size = 0x200; /* required amount of stack */

/* Specify the memory areas */
MEMORY
{
    FLASH (rx)     : ORIGIN = CONFIG_KERNEL_ADDR, LENGTH = 224K
    RAM (xrw)      : ORIGIN = 0x20000000, LENGTH = 64K
}

/* Define output sections */
SECTIONS
{
    /* The startup code goes first into FLASH */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector)) /* Startup code */
        . = ALIGN(4);
    } >FLASH

    /* The program code and other data goes into FLASH */
    .text :
    {
        . = ALIGN(4);
        *(.kernel_init)
        . = ALIGN(4);
        __task_data_start = .;
        KEEP (*(.task.data))
        __task_data_end = .;
        . = ALIGN(4);
        __console_data_start = .;
        KEEP (*(.console.data))
        __console_data_end = .;
        . = ALIGN(4);
        __init_data_start = .;
        KEEP (*(.core_init.data))
        KEEP (*(.early_init.data))
        KEEP (*(.normal_init.data))
        KEEP (*(.late_init.data))
        __init_data_end = .;
        . = ALIGN(4);
        *(.text)           /* .text sections (code) */
        *(.text*)          /* .text* sections (code) */
        *(.glue_7)         /* glue arm to thumb code */
        *(.glue_7t)        /* glue thumb to arm code */
        *(.eh_frame)

        KEEP (*(.init))
        KEEP (*(.fini))

        . = ALIGN(4);
        _etext = .;        /* define a global symbols at end of code */
    } >FLASH

    /* Constant data goes into FLASH */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)         /* .rodata sections (constants, strings, etc.) */
        *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
        . = ALIGN(4);
    } >FLASH

    .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
        .ARM : {
            __exidx_start = .;
            *(.ARM.exidx*)
            __exidx_end = .;
    } >FLASH

    .preinit_array     :
    {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array*))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } >FLASH
    .init_array :
    {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT(.init_array.*)))
        KEEP (*(.init_array*))
        PROVIDE_HIDDEN (__init_array_end = .);
    } >FLASH
    .fini_array :
    {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT(.fini_array.*)))
        KEEP (*(.fini_array*))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } >FLASH

    /* used by the startup to initialize data */
    _sidata = LOADADDR(.data);

    /* Initialized data sections goes into RAM, load LMA copy after code */
    .data :
    {
        . = ALIGN(4);
        . = ALIGN(CONFIG_PAGE_SIZE);
        __mm_sys_reserve_start = .;
        _sdata = .;        /* create a global symbol at data start */
        *(.data)           /* .data sections */
        *(.data*)          /* .data* sections */
        . = ALIGN(4);
        __memory_node_data_start = .;
        KEEP (*(.memory_node.data))
        __memory_node_data_end = .;
        . = ALIGN(4);
        _edata = .;        /* define a global symbol at data end */
    } >RAM AT> FLASH


    /* Uninitialized data section */
    . = ALIGN(4);
    .bss :
    {
        /* This is used by the startup in order to initialize the .bss secion */
        _sbss = .;         /* define a global symbol at bss start */
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)

        . = ALIGN(4);
        _ebss = .;         /* define a global symbol at bss end */
        __bss_end__ = _ebss;
    } >RAM

    . = ALIGN(CONFIG_PAGE_SIZE);
    __mm_sys_reserve_end = .;
    __mm_pool_start = .;
    __mm_pool_end = _estack - _Min_Stack_Size;

  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
