set(ARCH_ARM_SRCS
    hal/hal_arm.c
    kernel/irq.c
    kernel/switch.S
    kernel/atomic.c
    mm/mmu.c
    mm/mpu.c
    cpu/cpu.c
    init.c
)

if(CONFIG_ARM_CORTEX_M3 OR CONFIG_ARM_CORTEX_M4)
    list(APPEND ARCH_ARM_SRCS cortex_m/start.S)
else()
    message(FATAL_ERROR "ARM CPU ${CONFIG_CPU_TYPE} has no startup implementation")
endif()

if("${CONFIG_CPU_TYPE}" STREQUAL "cortex-m3")
    list(APPEND ARCH_ARM_SRCS
        cpu/cortex-m3/core_cm3.c
        cpu/cortex-m3/cpu_c.c
        cpu/cortex-m3/cpu_s.S
    )
elseif("${CONFIG_CPU_TYPE}" STREQUAL "cortex-m4")
    list(APPEND ARCH_ARM_SRCS
        cpu/cortex-m4/cpu_c.c
        cpu/cortex-m4/cpu_s.S
    )
endif()

add_library(nos_arch OBJECT ${ARCH_ARM_SRCS})
nos_configure_kernel_target(nos_arch)

if(CONFIG_SOC_STM32F1)
    target_include_directories(nos_arch PRIVATE "${CMAKE_SOURCE_DIR}/soc/stm32f1")
elseif(CONFIG_SOC_STM32F4)
    target_include_directories(nos_arch PRIVATE "${CMAKE_SOURCE_DIR}/soc/stm32f4")
elseif(CONFIG_SOC_NUC472)
    target_include_directories(nos_arch PRIVATE "${CMAKE_SOURCE_DIR}/soc/nuc472")
else()
    message(FATAL_ERROR "No Cortex-M IRQ vector table for the configured SoC")
endif()
