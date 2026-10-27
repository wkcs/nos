#
# NOS RISC-V Bare-Metal / Microkernel Toolchain (riscv64-unknown-elf-gcc)
#

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv)

if(NOT DEFINED TOOLCHAIN_PREFIX)
    find_program(_RISCV_GCC NAMES riscv64-unknown-elf-gcc riscv-none-elf-gcc riscv64-elf-gcc riscv64-linux-gnu-gcc)
    if(_RISCV_GCC MATCHES "riscv-none-elf-gcc$")
        set(TOOLCHAIN_PREFIX "riscv-none-elf-")
    elseif(_RISCV_GCC MATCHES "riscv64-elf-gcc$")
        set(TOOLCHAIN_PREFIX "riscv64-elf-")
    elseif(_RISCV_GCC MATCHES "riscv64-linux-gnu-gcc$")
        set(TOOLCHAIN_PREFIX "riscv64-linux-gnu-")
    else()
        set(TOOLCHAIN_PREFIX "riscv64-unknown-elf-")
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
