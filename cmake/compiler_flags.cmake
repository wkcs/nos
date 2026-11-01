#
# NOS Architecture, Board, and Compiler Flag Configuration
# Translates Kconfig CONFIG_* symbols into modular CMake lists (NOS_ARCH_FLAGS, NOS_COMMON_CFLAGS, etc.)
#

set(NOS_ARCH_FLAGS "")
set(NOS_FP_FLAGS "")
set(NOS_GLOBAL_DEFINES "")
set(NOS_OPTIMIZATION_FLAG -O2)

set(_NOS_ARCH_FLAGS_FILE "${CMAKE_SOURCE_DIR}/arch/${CONFIG_ARCH}/compiler_flags.cmake")
if(NOT EXISTS "${_NOS_ARCH_FLAGS_FILE}")
    message(FATAL_ERROR "No compiler flags for architecture: ${CONFIG_ARCH}")
endif()
include("${_NOS_ARCH_FLAGS_FILE}")

set(_NOS_BOARD_FLAGS_FILE "${CMAKE_SOURCE_DIR}/board/${CONFIG_BOARD}/compiler_flags.cmake")
if(DEFINED CONFIG_BOARD AND EXISTS "${_NOS_BOARD_FLAGS_FILE}")
    include("${_NOS_BOARD_FLAGS_FILE}")
endif()

# Base C and ASM compilation flags
set(NOS_COMMON_FLAGS
    ${NOS_ARCH_FLAGS}
    ${NOS_FP_FLAGS}
    ${NOS_OPTIMIZATION_FLAG}
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
    -Wl,--defsym=malloc_getpagesize_P=0x80
)
