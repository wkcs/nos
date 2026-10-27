set(USER_LIBNOS_SRCS
    libnos/ipc_client.c
    libnos/posix_fd.c
)

add_library(nos_user OBJECT ${USER_LIBNOS_SRCS})
nos_configure_kernel_target(nos_user)
