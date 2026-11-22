# 架构与 ABI 编译链接参数（由内核及固件构建共享）
list(APPEND NOS_GLOBAL_DEFINES "__ARM64__" "__aarch64__")

if(CONFIG_ARM64_CORTEX_A53)
    list(APPEND NOS_ARCH_FLAGS -march=armv8-a -mcpu=cortex-a53)
elseif(CONFIG_ARM64_CORTEX_A72)
    list(APPEND NOS_ARCH_FLAGS -march=armv8-a -mcpu=cortex-a72)
elseif(CONFIG_ARM64_CORTEX_A76)
    list(APPEND NOS_ARCH_FLAGS -march=armv8.2-a -mcpu=cortex-a76)
elseif(CONFIG_ARM64_GENERIC_V9)
    list(APPEND NOS_ARCH_FLAGS -march=armv9-a)
else()
    list(APPEND NOS_ARCH_FLAGS -march=armv8-a)
endif()

# 内核态禁止使用 FP/SIMD 寄存器，保障惰性向量切换零污染
if(NOT DEFINED CONFIG_ARM64_KERNEL_NO_FP OR CONFIG_ARM64_KERNEL_NO_FP)
    list(APPEND NOS_FP_FLAGS -mgeneral-regs-only)
endif()
