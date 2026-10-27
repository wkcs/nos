#
# NOS ARM 32-bit Bare-Metal / RTOS Toolchain (arm-none-eabi-gcc)
#

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED TOOLCHAIN_PREFIX)
    set(TOOLCHAIN_PREFIX "arm-none-eabi-")
endif()

# Support common Windows / macOS / Linux installation locations in addition to PATH
file(GLOB _WIN_ARM_GCC_DIRS
    "C:/Program Files (x86)/Arm GNU Toolchain*/*/bin"
    "C:/Program Files/Arm GNU Toolchain*/*/bin"
    "C:/Program Files (x86)/GNU Arm Embedded Toolchain/*/bin"
    "C:/ST/STM32CubeCLT*/GNU-tools-for-STM32/bin"
)
set(_TOOLCHAIN_HINTS
    "$ENV{ARM_GCC_PATH}"
    "$ENV{ARM_GCC_PATH}/bin"
    ${_WIN_ARM_GCC_DIRS}
)

find_program(CMAKE_C_COMPILER   NAMES "${TOOLCHAIN_PREFIX}gcc"     HINTS ${_TOOLCHAIN_HINTS} REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES "${TOOLCHAIN_PREFIX}g++"     HINTS ${_TOOLCHAIN_HINTS})
find_program(CMAKE_ASM_COMPILER NAMES "${TOOLCHAIN_PREFIX}gcc"     HINTS ${_TOOLCHAIN_HINTS} REQUIRED)
find_program(CMAKE_AR           NAMES "${TOOLCHAIN_PREFIX}ar"      HINTS ${_TOOLCHAIN_HINTS} REQUIRED)
find_program(CMAKE_OBJCOPY      NAMES "${TOOLCHAIN_PREFIX}objcopy" HINTS ${_TOOLCHAIN_HINTS} REQUIRED)
find_program(CMAKE_OBJDUMP      NAMES "${TOOLCHAIN_PREFIX}objdump" HINTS ${_TOOLCHAIN_HINTS} REQUIRED)
find_program(CMAKE_SIZE         NAMES "${TOOLCHAIN_PREFIX}size"    HINTS ${_TOOLCHAIN_HINTS} REQUIRED)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
