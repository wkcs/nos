set(FS_SRCS
    vfs.c
    vfs_fd.c
    vfs_io.c
    ramfs.c
    procfs.c
    sysfs.c
)

if(CONFIG_VFS_SMOKE_TEST OR (CONFIG_FATFS AND CONFIG_BOARD_QEMU_STM32))
    list(APPEND FS_SRCS vfs_smoke.c)
endif()

if(CONFIG_VFS_FD_CONCURRENCY_TEST)
    list(APPEND FS_SRCS vfs_fd_test.c)
endif()

if(CONFIG_VFS_FAT_ALIAS_TEST)
    list(APPEND FS_SRCS vfs_alias_test.c)
endif()

if(CONFIG_FATFS)
    list(APPEND FS_SRCS
        fatfs/diskio.c
        fatfs/ff.c
        fatfs/sync.c
        fatfs/option/unicode.c
        fatfs/fatfs_vfs.c
    )
endif()

add_library(nos_fs OBJECT ${FS_SRCS})
nos_configure_kernel_target(nos_fs)
