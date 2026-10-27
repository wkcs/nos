#
# NOS ARM64 Bare-Metal / Microkernel Toolchain (aarch64-none-elf-gcc / clang)
#

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED TOOLCHAIN_PREFIX)
    find_program(_AARCH64_GCC NAMES aarch64-none-elf-gcc aarch64-elf-gcc aarch64-linux-gnu-gcc)
    if(_AARCH64_GCC MATCHES "aarch64-none-elf-gcc$")
        set(TOOLCHAIN_PREFIX "aarch64-none-elf-")
    elseif(_AARCH64_GCC MATCHES "aarch64-elf-gcc$")
        set(TOOLCHAIN_PREFIX "aarch64-elf-")
    else()
        set(TOOLCHAIN_PREFIX "aarch64-linux-gnu-")
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
