set(FS_SRCS
    vfs.c
    vfs_fd.c
    vfs_io.c
    ramfs.c
    procfs.c
    sysfs.c
)

if(CONFIG_FATFS OR CONFIG_VFS_SMOKE_TEST)
    list(APPEND FS_SRCS vfs_smoke.c)
endif()

if(CONFIG_VFS_FD_CONCURRENCY_TEST)
    list(APPEND FS_SRCS vfs_fd_test.c)
endif()

if(CONFIG_FATFS)
    list(APPEND FS_SRCS
        fatfs/diskio.c
        fatfs/ff.c
        fatfs/sync.c
        fatfs/option/cc936.c
        fatfs/fatfs_vfs.c
    )
endif()

add_library(nos_fs OBJECT ${FS_SRCS})
nos_configure_kernel_target(nos_fs)
