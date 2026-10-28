add_library(nos_libc OBJECT
    port/picolibc_newlib.c
)

nos_configure_kernel_target(nos_libc)
