#
# NOS Multi-Target Clang/LLVM Cross Toolchain
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/clang-cross.cmake -DNOS_TARGET_TRIPLE=arm-none-eabi ...
#

set(CMAKE_SYSTEM_NAME Generic)

if(NOT DEFINED NOS_TARGET_TRIPLE)
    set(NOS_TARGET_TRIPLE "arm-none-eabi")
endif()

set(CMAKE_C_COMPILER            clang)
set(CMAKE_C_COMPILER_TARGET     "${NOS_TARGET_TRIPLE}")
set(CMAKE_CXX_COMPILER          clang++)
set(CMAKE_CXX_COMPILER_TARGET   "${NOS_TARGET_TRIPLE}")
set(CMAKE_ASM_COMPILER          clang)
set(CMAKE_ASM_COMPILER_TARGET   "${NOS_TARGET_TRIPLE}")

find_program(CMAKE_AR      NAMES llvm-ar "${NOS_TARGET_TRIPLE}-ar" REQUIRED)
find_program(CMAKE_OBJCOPY NAMES llvm-objcopy "${NOS_TARGET_TRIPLE}-objcopy" REQUIRED)
find_program(CMAKE_OBJDUMP NAMES llvm-objdump "${NOS_TARGET_TRIPLE}-objdump" REQUIRED)
find_program(CMAKE_SIZE    NAMES llvm-size "${NOS_TARGET_TRIPLE}-size" REQUIRED)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
