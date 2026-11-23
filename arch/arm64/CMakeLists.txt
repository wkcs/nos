add_library(nos_arch OBJECT
    boot/start.S
    cpu/cpu.c
    cpu/cpufeature.c
    cpu/fpu.c
    cpu/psci.c
    cpu/security.c
    cpu/smp.c
    hal/hal_arm64.c
    init.c
    kernel/atomic.c
    kernel/el0_smoke.S
    kernel/fdt_scan.c
    kernel/irq.c
    kernel/percpu.c
    kernel/sve.S
    kernel/switch.S
    kernel/task.c
    kernel/timer.c
    kernel/trap.c
    kernel/uaccess.S
    kernel/uaccess.c
    kernel/vectors.S
    mm/fixmap.c
    mm/mmu.c
)
if(CONFIG_NOS_AI_CORE)
    target_sources(nos_arch PRIVATE mm/smmuv3.c)
endif()
if(CONFIG_ARM64_STAGE2_SELFTEST)
    target_sources(nos_arch PRIVATE kernel/stage2_selftest.c)
endif()
nos_configure_kernel_target(nos_arch)
