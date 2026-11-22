if(NOS_HAS_TOOLCHAIN_LIBC)
    set(NOS_LIBC_SRCS port/picolibc_newlib.c)
else()
    set(NOS_LIBC_SRCS freestanding/klibc_runtime.c)
endif()

add_library(nos_libc OBJECT
    ${NOS_LIBC_SRCS}
)

nos_configure_kernel_target(nos_libc)
