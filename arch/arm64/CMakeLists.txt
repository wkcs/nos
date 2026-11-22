add_library(nos_arch OBJECT
    boot/start.S
    cpu/cpu.c
    cpu/cpufeature.c
    cpu/fpu.c
    cpu/psci.c
    cpu/smp.c
    hal/hal_arm64.c
    init.c
    kernel/atomic.c
    kernel/el0_smoke.S
    kernel/irq.c
    kernel/percpu.c
    kernel/switch.S
    kernel/task.c
    kernel/timer.c
    kernel/trap.c
    kernel/vectors.S
    mm/fixmap.c
    mm/mmu.c
)
if(CONFIG_ARM64_STAGE2_SELFTEST)
    target_sources(nos_arch PRIVATE kernel/stage2_selftest.c)
endif()
nos_configure_kernel_target(nos_arch)
