#
# NOS x86_64 Bare-Metal / Microkernel Toolchain (x86_64-elf-gcc or clang)
#

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED TOOLCHAIN_PREFIX)
    find_program(_X86_GCC NAMES x86_64-elf-gcc x86_64-linux-gnu-gcc)
    if(_X86_GCC MATCHES "x86_64-elf-gcc$")
        set(TOOLCHAIN_PREFIX "x86_64-elf-")
    else()
        set(TOOLCHAIN_PREFIX "x86_64-linux-gnu-")
    endif()
endif()

set(CMAKE_C_COMPILER   "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}g++")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_AR           "${TOOLCHAIN_PREFIX}ar")
set(CMAKE_OBJCOPY      "${TOOLCHAIN_PREFIX}objcopy")
set(CMAKE_OBJDUMP      "${TOOLCHAIN_PREFIX}objdump")
set(CMAKE_SIZE         "${TOOLCHAIN_PREFIX}size")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
