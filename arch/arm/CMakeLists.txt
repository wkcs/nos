set(ARCH_ARM_SRCS
    boot/start.S
    hal/hal_arm.c
    kernel/irq.c
    kernel/switch.S
    kernel/atomic.c
    mm/mmu.c
    cpu/cpu.c
    init.c
)

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
