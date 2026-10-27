add_library(nos_init OBJECT
    main.c
    init.c
)
nos_configure_kernel_target(nos_init)
