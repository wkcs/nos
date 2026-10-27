add_library(nos_arch OBJECT
    init.c
    hal/hal_riscv.c
)
nos_configure_kernel_target(nos_arch)
