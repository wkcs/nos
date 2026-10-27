#
# NOS Architecture, Board, and Compiler Flag Configuration
# Translates Kconfig CONFIG_* symbols into modular CMake lists (NOS_ARCH_FLAGS, NOS_COMMON_CFLAGS, etc.)
#

set(NOS_ARCH_FLAGS "")
set(NOS_FP_FLAGS "")
set(NOS_GLOBAL_DEFINES "")

if(CONFIG_ARCH_ARM)
    list(APPEND NOS_GLOBAL_DEFINES "__ARM__")
    if(CONFIG_ARM_CORTEX_M3)
        list(APPEND NOS_ARCH_FLAGS -march=armv7-m -mcpu=cortex-m3)
        list(APPEND NOS_FP_FLAGS -msoft-float)
    elseif(CONFIG_ARM_CORTEX_M4)
        list(APPEND NOS_ARCH_FLAGS -march=armv7e-m -mcpu=cortex-m4)
        if(CONFIG_ARM_CORTEX_M4_FPU)
            list(APPEND NOS_FP_FLAGS -mfloat-abi=hard -mfpu=fpv4-sp-d16)
            list(APPEND NOS_GLOBAL_DEFINES "__VFP_FP__")
        else()
            list(APPEND NOS_FP_FLAGS -msoft-float)
        endif()
    elseif(CONFIG_ARM_CORTEX_A7)
        list(APPEND NOS_ARCH_FLAGS -march=armv7-a -mcpu=cortex-a7)
        list(APPEND NOS_FP_FLAGS -mfloat-abi=hard -mfpu=neon-vfpv4)
    else()
        list(APPEND NOS_ARCH_FLAGS -march=armv7-m)
    endif()

    if(CONFIG_THUMB2_KERNEL)
        list(APPEND NOS_ARCH_FLAGS -mthumb)
        list(APPEND NOS_GLOBAL_DEFINES "__THUMB2__")
    endif()
elseif(CONFIG_ARCH_ARM64)
    list(APPEND NOS_GLOBAL_DEFINES "__ARM64__" "__aarch64__")
    if(CONFIG_ARM64_CORTEX_A53)
        list(APPEND NOS_ARCH_FLAGS -march=armv8-a -mcpu=cortex-a53)
    elseif(CONFIG_ARM64_CORTEX_A72)
        list(APPEND NOS_ARCH_FLAGS -march=armv8-a -mcpu=cortex-a72)
    else()
        list(APPEND NOS_ARCH_FLAGS -march=armv8-a)
    endif()
elseif(CONFIG_ARCH_RISCV)
    list(APPEND NOS_GLOBAL_DEFINES "__RISCV__")
    if(CONFIG_RISCV_32IMAC)
        list(APPEND NOS_ARCH_FLAGS -march=rv32imac -mabi=ilp32)
    else()
        list(APPEND NOS_ARCH_FLAGS -march=rv64gc -mabi=lp64d -mcmodel=medany)
    endif()
elseif(CONFIG_ARCH_X86)
    list(APPEND NOS_GLOBAL_DEFINES "__X86_64__")
    list(APPEND NOS_ARCH_FLAGS -m64 -mno-red-zone -mcmodel=kernel -fno-pic -fno-pie)
endif()

# Board-specific chip family macros
if("${CONFIG_BOARD}" STREQUAL "nk60-v2")
    list(APPEND NOS_GLOBAL_DEFINES "STM32F401xx" "USE_STDPERIPH_DRIVER")
elseif("${CONFIG_BOARD}" STREQUAL "qemu-stm32")
    list(APPEND NOS_GLOBAL_DEFINES "STM32F40_41xxx" "USE_STDPERIPH_DRIVER")
elseif("${CONFIG_BOARD}" STREQUAL "wl-001")
    list(APPEND NOS_GLOBAL_DEFINES "STM32F10X_MD" "USE_STDPERIPH_DRIVER")
elseif("${CONFIG_BOARD}" STREQUAL "zj-v3")
    list(APPEND NOS_GLOBAL_DEFINES "STM32F10X_HD" "USE_STDPERIPH_DRIVER")
endif()

# Base C and ASM compilation flags
set(NOS_COMMON_FLAGS
    ${NOS_ARCH_FLAGS}
    ${NOS_FP_FLAGS}
    -O2
    -g3
    -fmessage-length=0
    -fsigned-char
    -ffunction-sections
    -fdata-sections
    -ffreestanding
    -fno-omit-frame-pointer
)

set(NOS_C_FLAGS
    ${NOS_COMMON_FLAGS}
    -std=gnu11
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-array-bounds
)

set(NOS_ASM_FLAGS
    ${NOS_COMMON_FLAGS}
    -Wall
    -Wextra
    -Werror
    -x assembler-with-cpp
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND NOS_C_FLAGS
        -Wno-address-of-packed-member
        -Wno-gnu-variable-sized-type-not-at-end
        -Wno-format-invalid-specifier
        -Wno-tautological-constant-out-of-range-compare
        -Wno-unaligned-access
        -Wno-initializer-overrides
        -Wno-format-security
        -Wno-sign-compare
        -Wno-format-zero-length
        -fno-builtin-memcpy
        -fno-builtin-memset
        -fno-builtin-memmove
        -fno-addrsig
        -fno-jump-tables
    )
    if(NOT CONFIG_ARCH_ARM64)
        list(APPEND NOS_C_FLAGS -mno-global-merge)
    endif()
else()
    # GCC-specific flags
    list(APPEND NOS_C_FLAGS
        -fno-move-loop-invariants
        -fno-delete-null-pointer-checks
        -fno-var-tracking-assignments
        -fno-strict-overflow
        -fconserve-stack
        -Wno-maybe-uninitialized
        -Wno-unused-but-set-variable
        -Wno-stringop-truncation
        -fno-tree-loop-im
        -fno-merge-all-constants
    )
    list(APPEND NOS_ASM_FLAGS -fno-move-loop-invariants)
    if(CONFIG_ARCH_ARM)
        list(APPEND NOS_C_FLAGS -mno-sched-prolog -mapcs-frame -Wa,-mimplicit-it=thumb)
        list(APPEND NOS_ASM_FLAGS -mno-sched-prolog -Wa,-mimplicit-it=thumb)
    endif()
endif()

set(NOS_LINK_FLAGS
    ${NOS_ARCH_FLAGS}
    ${NOS_FP_FLAGS}
    -nostdinc
    -Wl,--gc-sections
    -Wl,--no-warn-rwx-segments
    -Wl,-cref
    -Wl,-u,Reset_Handler
    -Wl,--defsym=malloc_getpagesize_P=0x80
)
