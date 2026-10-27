add_library(nos_arch OBJECT
    init.c
    hal/hal_arm64.c
)
nos_configure_kernel_target(nos_arch)
