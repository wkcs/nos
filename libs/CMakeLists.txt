add_library(nos_libs OBJECT
    string.c
    vsprintf.c
    kfifo.c
)
nos_configure_kernel_target(nos_libs)
